/*
 * controller.cpp - tinyg2 controller and top level parser
 * Part of TinyG2 project
 *
 * Copyright (c) 2013 Alden S. Hart Jr. 
 * Copyright (c) 2013 Robert Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License, version 2 as published by the 
 * Free Software Foundation. You should have received a copy of the GNU General Public 
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * As a special exception, you may use this file as part of a software library without 
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other 
 * files to produce an executable, this file does not by itself cause the resulting 
 * executable to be covered by the GNU General Public License. This exception does not 
 * however invalidate any other reasons why the executable file might be covered by the 
 * GNU General Public License. 
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY 
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT 
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF 
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* See the wiki for module details and additional information:
 * https://github.com/synthetos/g2/wiki
 */

#include "tinyg2.h"
#include "controller.h"
#include "config.h"
#include "config_app.h"
#include "json_parser.h"
#include "text_parser.h"
#include "gcode_parser.h"
#include "canonical_machine.h"
#include "plan_arc.h"
#include "planner.h"
#include "stepper.h"
#include "switch.h"
#include "hardware.h"
#include "report.h"
#include "help.h"
#include "xio.h"
#include "util.h"

/***********************************************************************************
 **** STUCTURE ALLOCATIONS *********************************************************
 ***********************************************************************************/

controller_t cs;		// controller state structure

/***********************************************************************************
 **** STATICS AND LOCALS ***********************************************************
 ***********************************************************************************/

static void _controller_HSM(void);
static stat_t _alarm_idler(void);
static stat_t _normal_idler(void);
static stat_t _limit_switch_handler(void);
//static stat_t _system_assertions(void);
//static stat_t _cycle_start_handler(void);
static stat_t _sync_to_planner(void);
//static stat_t _sync_to_tx_buffer(void);
static stat_t _command_dispatch(void);

// prep for export to other modules:
stat_t hardware_hard_reset_handler(void);
//stat_t hardware_bootloader_handler(void);

/***********************************************************************************
 **** CODE *************************************************************************
 ***********************************************************************************/
/*
 * controller_init() - controller init
 */

void controller_init(uint8_t std_in, uint8_t std_out, uint8_t std_err) 
{
	cs.magic_start = MAGICNUM;
	cs.magic_end = MAGICNUM;
	cs.fw_build = TINYG_FIRMWARE_BUILD;
	cs.fw_version = TINYG_FIRMWARE_VERSION;
	cs.hw_platform = TINYG_HARDWARE_PLATFORM;	// NB: HW version is set from EEPROM
	
	cs.linelen = 0;									// initialize index for read_line()
	cs.controller_state = CONTROLLER_NOT_CONNECTED;	// find USB next
//	cs.reset_requested = false;
//	cs.bootloader_requested = false;

//	xio_set_stdin(std_in);
//	xio_set_stdout(std_out);
//	xio_set_stderr(std_err);
//	cs.default_src = std_in;
//	tg_set_active_source(cs.default_src);	// set active source
}

/* 
 * controller_run() - MAIN LOOP - top-level controller
 *
 * The order of the dispatched tasks is very important. 
 * Tasks are ordered by increasing dependency (blocking hierarchy).
 * Tasks that are dependent on completion of lower-level tasks must be
 * later in the list than the task(s) they are dependent upon. 
 *
 * Tasks must be written as continuations as they will be called repeatedly, 
 * and are called even if they are not currently active. 
 *
 * The DISPATCH macro calls the function and returns to the controller parent 
 * if not finished (STAT_EAGAIN), preventing later routines from running 
 * (they remain blocked). Any other condition - OK or ERR - drops through 
 * and runs the next routine in the list.
 *
 * A routine that had no action (i.e. is OFF or idle) should return STAT_NOOP
 */

void controller_run() 
{ 
	while (true) { 
		_controller_HSM();
	}
}

#define	DISPATCH(func) if (func == STAT_EAGAIN) return; 
static void _controller_HSM()
{
//----- Interrupt Service Routines are the highest priority controller functions ----//
//      See hardware.h for a list of ISRs and their priorities.
//
//----- lowest level functions -------------------------------------------------------//
													// Order is important:
	DISPATCH(hw_hard_reset_handler());				// 1. received hard reset request
//	DISPATCH(hw_bootloader_handler());				// 2. received request to start bootloader
	DISPATCH(_alarm_idler());						// 3. idle in alarm state (shutdown)
	DISPATCH( poll_switches());						// 4. run a switch polling cycle
	DISPATCH(_limit_switch_handler());				// 5. limit switch has been thrown

	DISPATCH(cm_feedhold_sequencing_callback());	// 6a. feedhold state machine runner
	DISPATCH(mp_plan_hold_callback());				// 6b. plan a feedhold from line runtime
//	DISPATCH(_cycle_start_handler());				// 7. cycle start requested
//	DISPATCH(_system_assertions());					// 8. system integrity assertions

//----- planner hierarchy for gcode and cycles ---------------------------------------//

	DISPATCH(st_motor_power_callback());			// stepper motor disable timer
//	DISPATCH(switch_debounce_callback());			// debounce switches
	DISPATCH(sr_status_report_callback());			// conditionally send status report
	DISPATCH(qr_queue_report_callback());			// conditionally send queue report
	DISPATCH(cm_arc_callback());					// arc generation runs behind lines
	DISPATCH(cm_homing_callback());					// G28.2 continuation
//	DISPATCH(cm_probe_callback());					// G38.2 continuation

//----- command readers and parsers --------------------------------------------------//

	DISPATCH(_sync_to_planner());					// ensure there is at least one free buffer in planning queue
//	DISPATCH(_sync_to_tx_buffer());					// sync with TX buffer (pseudo-blocking)
//	DISPATCH(cfg_baud_rate_callback());				// perform baud rate update (must be after TX sync)
	DISPATCH(_command_dispatch());					// read and execute next command
	DISPATCH(_normal_idler());						// blink LEDs slowly to show everything is OK
}

/***************************************************************************** 
 * _command_dispatch() - dispatch line received from active input device
 *
 *	Reads next command line and dispatches to relevant parser or action
 *	Accepts commands if the move queue has room - EAGAINS if it doesn't
 *	Manages cutback to serial input from file devices (EOF)
 *	Also responsible for prompts and for flow control 
 */

static stat_t _command_dispatch()
{
	// detect USB connection and transition to disconnected state if it disconnected
	if (SerialUSB.isConnected() == false) cs.controller_state = CONTROLLER_NOT_CONNECTED;

	// read input line or return if not a completed line
	if (cs.controller_state == CONTROLLER_READY) {
		if (read_line(cs.in_buf, &cs.linelen, sizeof(cs.in_buf)) != STAT_OK) {
			return (STAT_OK);	// returns OK for anything NOT OK, so the idler always runs
		}

	} else if (cs.controller_state == CONTROLLER_NOT_CONNECTED) {
		if (SerialUSB.isConnected() == false) return (STAT_OK);
		cm_request_queue_flush();
		rpt_print_system_ready_message();
		cs.controller_state = CONTROLLER_STARTUP;

	} else if (cs.controller_state == CONTROLLER_STARTUP) {		// run startup code
//		strcpy(cs.in_buf, "$x");
//		strcpy(cs.in_buf, "g1f400x100");
//		strcpy(cs.in_buf, "?");
		cs.controller_state = CONTROLLER_READY;

	} else {
		return (STAT_OK);
	}
	
	// execute the text line
	strncpy(cs.saved_buf, cs.in_buf, SAVED_BUFFER_LEN-1);	// save input buffer for reporting
	cs.linelen = 0;
	// dispatch the new text line
	switch (toupper(cs.in_buf[0])) {

		case NUL: { 							// blank line (just a CR)
			if (cfg.comm_mode != JSON_MODE) {
				text_response(STAT_OK, cs.saved_buf);
			}
			break;
		}
		case 'H': { 							// intercept help screens
			cfg.comm_mode = TEXT_MODE;
			help_general(NULL);
			text_response(STAT_OK, cs.in_buf);
			break;
		}
		case '$': case '?':{ 					// text-mode configs
			cfg.comm_mode = TEXT_MODE;
			text_response(text_parser(cs.in_buf), cs.saved_buf);
			break;
		}
		case '{': { 							// JSON input
			cfg.comm_mode = JSON_MODE;
			json_parser(cs.in_buf);
			break;
		}
		default: {								// anything else must be Gcode
			if (cfg.comm_mode == JSON_MODE) {
				strncpy(cs.out_buf, cs.in_buf, INPUT_BUFFER_LEN -8);	// use out_buf as temp
				sprintf((char *)cs.in_buf,"{\"gc\":\"%s\"}\n", (char *)cs.out_buf);		// '-8' is used for JSON chars
				json_parser(cs.in_buf);
			} else {
				text_response(gc_gcode_parser(cs.in_buf), cs.saved_buf);
			}
		}
	}
	return (STAT_OK);
}

/**** Local Utilities ********************************************************/
/*
 * _alarm_idler() - blink rapidly and prevent further activity from occurring
 * _normal_idler() - blink Indicator LED slowly to show everything is OK
 *
 *	Alarm idler flashes indicator LED rapidly to show everything is not OK. 
 *	Alarm function returns EAGAIN causing the control loop to never advance beyond 
 *	this point. It's important that the reset handler is still called so a SW reset 
 *	(ctrl-x) or bootloader request can be processed.
 */

static stat_t _alarm_idler(  )
{
	if (cm_get_machine_state() != MACHINE_ALARM) { return (STAT_OK);}

	if (SysTickTimer.getValue() > cs.led_timer) {
		cs.led_timer = SysTickTimer.getValue() + LED_ALARM_TIMER;
		IndicatorLed.toggle();
	}
	return (STAT_EAGAIN);	// EAGAIN prevents any lower-priority actions from running
}

static stat_t _normal_idler(  )
{
	if (SysTickTimer.getValue() > cs.led_timer) {
		cs.led_timer = SysTickTimer.getValue() + LED_NORMAL_TIMER;
		IndicatorLed.toggle();
	}
	return (STAT_OK);
}

/*
 * tg_reset_source() 		 - reset source to default input device (see note)
 * tg_set_primary_source() 	 - set current primary input source
 * tg_set_secondary_source() - set current primary input source
 *
 * Note: Once multiple serial devices are supported reset_source() should
 * be expanded to also set the stdout/stderr console device so the prompt
 * and other messages are sent to the active device.
 */
/*
void tg_reset_source() { tg_set_primary_source(tg.default_src);}
void tg_set_primary_source(uint8_t dev) { tg.primary_src = dev;}
void tg_set_secondary_source(uint8_t dev) { tg.secondary_src = dev;}
*/

/*
 * _sync_to_tx_buffer() - return eagain if TX queue is backed up
 * _sync_to_planner() - return eagain if planner is not ready for a new command
 */
/*
static stat_t _sync_to_tx_buffer()
{
	if ((xio_get_tx_bufcount_usart(ds[XIO_DEV_USB].x) >= XOFF_TX_LO_WATER_MARK)) {
		return (STAT_EAGAIN);
	}
	return (STAT_OK);
}
*/

static stat_t _sync_to_planner()
{
if (mp_get_planner_buffers_available() < PLANNER_BUFFER_HEADROOM) {
	return (STAT_EAGAIN);
	}
	return (STAT_OK);
}

/*
 * _limit_switch_handler() - shut down system if limit switch fired
 */
static uint8_t _limit_switch_handler(void)
{
/*
	if (cm_get_machine_state() == MACHINE_ALARM) { return (STAT_NOOP);}
	if (cm.limit_tripped_flag == false) { return (STAT_NOOP);}
	cm.limit_tripped_flag = false;
//	cm_alarm(0);
*/
	return (STAT_OK);
}

/* 
 * _system_assertions() - check memory integrity and other assertions
 */
/*
static stat_t _system_assertions()
{
	uint8_t value = 0;
	
	if (cs.magic_start		!= MAGICNUM) { value = 1; }		// Note: reported VALue is offset by ALARM_MEMORY_OFFSET
	if (cs.magic_end		!= MAGICNUM) { value = 2; }
	if (cm.magic_start 		!= MAGICNUM) { value = 3; }
	if (cm.magic_end		!= MAGICNUM) { value = 4; }
	if (gm.magic_start		!= MAGICNUM) { value = 5; }
	if (gm.magic_end 		!= MAGICNUM) { value = 6; }
	if (cfg.magic_start		!= MAGICNUM) { value = 7; }
	if (cfg.magic_end		!= MAGICNUM) { value = 8; }
	if (cmdStr.magic_start	!= MAGICNUM) { value = 9; }
	if (cmdStr.magic_end	!= MAGICNUM) { value = 10; }
	if (mb.magic_start		!= MAGICNUM) { value = 11; }
	if (mb.magic_end		!= MAGICNUM) { value = 12; }
	if (mr.magic_start		!= MAGICNUM) { value = 13; }
	if (mr.magic_end		!= MAGICNUM) { value = 14; }
	if (ar.magic_start		!= MAGICNUM) { value = 15; }
	if (ar.magic_end		!= MAGICNUM) { value = 16; }
	if (st_get_st_magic()	!= MAGICNUM) { value = 17; }
	if (st_get_sps_magic()	!= MAGICNUM) { value = 18; }
	if (rtc.magic_end 		!= MAGICNUM) { value = 19; }
	xio_assertions(&value);									// run xio assertions

	if (value == 0) { return (STAT_OK);}
	rpt_exception(STAT_MEMORY_CORRUPTION, value);
	cm_alarm(ALARM_MEMORY_OFFSET + value);	
	return (STAT_EAGAIN);
}
*/
