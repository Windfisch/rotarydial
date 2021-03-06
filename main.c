/*
    Project: Rotary-Dial-To-USB Interface
    Author: Florian Jung (flo@windfisch.org)
    Copyright: (c) 2014 by Florian Jung and partially others (see below)
    License: GNU GPL v3 (see LICENSE)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3 as 
    published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.


    This project is based on the code for the AVR ATtiny USB Tutorial at
    http://codeandlife.com/ by Joonas Pihlajamaa, joonas.pihlajamaa@iki.fi,
    which is in turn based on the V-USB example code by Christian Starkjohann
    (Copyright: (c) 2008 by OBJECTIVE DEVELOPMENT Software GmbH)
*/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <util/delay.h>

#include "usbdrv/usbdrv.h"

// ************************
// *** USB HID ROUTINES ***
// ************************

// From Frank Zhao's USB Business Card project
// http://www.frank-zhao.com/cache/usbbusinesscard_details.php
PROGMEM const char
	usbHidReportDescriptor[USB_CFG_HID_REPORT_DESCRIPTOR_LENGTH] = {
	0x05, 0x01,					// USAGE_PAGE (Generic Desktop)
	0x09, 0x06,					// USAGE (Keyboard)
	0xa1, 0x01,					// COLLECTION (Application)
	0x75, 0x01,					//   REPORT_SIZE (1)
	0x95, 0x08,					//   REPORT_COUNT (8)
	0x05, 0x07,					//   USAGE_PAGE (Keyboard)(Key Codes)
	0x19, 0xe0,					//   USAGE_MINIMUM (Keyboard LeftControl)(224)
	0x29, 0xe7,					//   USAGE_MAXIMUM (Keyboard Right GUI)(231)
	0x15, 0x00,					//   LOGICAL_MINIMUM (0)
	0x25, 0x01,					//   LOGICAL_MAXIMUM (1)
	0x81, 0x02,					//   INPUT (Data,Var,Abs) ; Modifier byte
	0x95, 0x01,					//   REPORT_COUNT (1)
	0x75, 0x08,					//   REPORT_SIZE (8)
	0x81, 0x03,					//   INPUT (Cnst,Var,Abs) ; Reserved byte
	0x95, 0x05,					//   REPORT_COUNT (5)
	0x75, 0x01,					//   REPORT_SIZE (1)
	0x05, 0x08,					//   USAGE_PAGE (LEDs)
	0x19, 0x01,					//   USAGE_MINIMUM (Num Lock)
	0x29, 0x05,					//   USAGE_MAXIMUM (Kana)
	0x91, 0x02,					//   OUTPUT (Data,Var,Abs) ; LED report
	0x95, 0x01,					//   REPORT_COUNT (1)
	0x75, 0x03,					//   REPORT_SIZE (3)
	0x91, 0x03,					//   OUTPUT (Cnst,Var,Abs) ; LED report padding
	0x95, 0x06,					//   REPORT_COUNT (6)
	0x75, 0x08,					//   REPORT_SIZE (8)
	0x15, 0x00,					//   LOGICAL_MINIMUM (0)
	0x25, 0x65,					//   LOGICAL_MAXIMUM (101)
	0x05, 0x07,					//   USAGE_PAGE (Keyboard)(Key Codes)
	0x19, 0x00,					//   USAGE_MINIMUM (Reserved (no event indicated))(0)
	0x29, 0x65,					//   USAGE_MAXIMUM (Keyboard Application)(101)
	0x81, 0x00,					//   INPUT (Data,Ary,Abs)
	0xc0						// END_COLLECTION
};

typedef struct
{
	uint8_t modifier;
	uint8_t reserved;
	uint8_t keycode[6];
} keyboard_report_t;

static keyboard_report_t keyboard_report;	// sent to PC
volatile static uchar LED_state = 0xff;	// received from PC
static uchar idleRate;			// repeat rate for keyboards


#define LED_BLUE (1<<5)
#define LED_RED (1<<4)
#define LED_GREEN (1<<3)

usbMsgLen_t usbFunctionSetup(uchar data[8])
{
	usbRequest_t *rq = (void *) data;

	if ((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_CLASS)
	{
		switch (rq->bRequest)
		{
			case USBRQ_HID_GET_REPORT:	// send "no keys pressed" if asked here
				// wValue: ReportType (highbyte), ReportID (lowbyte)
				usbMsgPtr = (void *) &keyboard_report;	// we only have this one
				keyboard_report.modifier = 0;
				keyboard_report.keycode[0] = 0;
				return sizeof(keyboard_report);

			case USBRQ_HID_SET_REPORT:	// if wLength == 1, should be LED state
				return (rq->wLength.word == 1) ? USB_NO_MSG : 0;
			
			case USBRQ_HID_GET_IDLE:	// send idle rate to PC as required by spec
				usbMsgPtr = &idleRate;
				return 1;
			
			case USBRQ_HID_SET_IDLE:	// save idle rate as required by spec
				idleRate = rq->wValue.bytes[1];
				return 0;
		}
	}

	return 0;					// by default don't return any data
}

usbMsgLen_t usbFunctionWrite(uint8_t * data, uchar len)
{
	if (data[0] == LED_state)
		return 1;
	else
		LED_state = data[0];

	return 1;					// Data read, not expecting more
}

// Now only supports letters 'a' to 'z' and 0 (NULL) to clear buttons
void buildReport(int n)
{
	keyboard_report.modifier = 0;

	if (n > 0 && n <= 10)
		keyboard_report.keycode[0] = 29+n;
	else if (n == 0)
		keyboard_report.keycode[0] = 39;
	else if (n == -2)
		keyboard_report.keycode[0] = 40; // enter
	else if (n == -1)
		keyboard_report.keycode[0] = 0;
}

#define STATE_WAIT 0
#define STATE_SEND_KEY 1
#define STATE_RELEASE_KEY 2

void jump_to_bootloader(void)
{
	cli();
	wdt_enable(WDTO_15MS);
	while (1);
}


#define DEBOUNCE_MAX 100
uint8_t idle_stable = 42;
uint8_t pulse_stable = 42;
uint16_t idlectr = DEBOUNCE_MAX;
uint16_t pulsectr = DEBOUNCE_MAX;

uint8_t oldidle = 42;
uint8_t oldpulse = 42;

int main()
{
	int key_to_send;
	int n_pulses = 0;

	uchar i, button_release_counter = 0, state = STATE_WAIT;

	DDRC = 0x38;   // LEDs as output
	PORTC |= LED_BLUE | LED_RED | LED_GREEN;
	DDRD &= ~0xF3; // connector ports as input
	DDRB &= ~0x3C;
	PORTD &= ~0xF3; // disable pullups for unused ports
	PORTB &= ~0x0C;
	PORTB |= 0x30; // enable pullups for PB4 and 5

	cli();

	for (i = 0; i < sizeof(keyboard_report); i++)	// clear report initially
		((uchar *) & keyboard_report)[i] = 0;

	wdt_enable(WDTO_1S);		// enable 1s watchdog timer

	usbInit();

	usbDeviceDisconnect();		// enforce re-enumeration
	for (i = 0; i < 250; i++)
	{							// wait 500 ms
		wdt_reset();			// keep the watchdog happy
		_delay_ms(10);
	}
	usbDeviceConnect();

	sei();						// Enable interrupts after re-enumeration
	uint32_t j;

	while (1)
	{
		wdt_reset();			// keep the watchdog happy
		usbPoll();


		// characters are sent when messageState == STATE_SEND and after receiving
		// the initial LED state from PC (good way to wait until device is recognized)
		
		int idle = (PINB & 0x10);
		int pulse = (PINB & 0x20);

		if (idle != idle_stable)
		{
			idlectr++;
			if (idlectr > DEBOUNCE_MAX)
				idle_stable = idle;
		}
		else
			idlectr=0;
		
		if (pulse != pulse_stable)
		{
			pulsectr++;
			if (pulsectr > DEBOUNCE_MAX)
				pulse_stable = pulse;
		}
		else
			pulsectr=0;

		if (pulse_stable)
			PORTC &= ~LED_RED;
		else
			PORTC |= LED_RED; 

		if (idle_stable)
			PORTC |= LED_GREEN;
		else
			PORTC &= ~LED_GREEN;


		if (oldidle == 42) // init
		{
			oldidle = idle_stable;
			oldpulse = pulse_stable;
		}

		if (oldidle && !idle_stable)
			n_pulses = 0;

		if (!oldidle == 0 && idle_stable)
		{
			if (n_pulses > 17)
				jump_to_bootloader();

			// also check if some time has elapsed since last button press
			if (state == STATE_WAIT && button_release_counter == 255)
			{
				if (n_pulses >= 1 && n_pulses <=10)
				{
					state = STATE_SEND_KEY;
					key_to_send = n_pulses;
				}
			}

			button_release_counter = 0;	// now button needs to be released a while until retrigger

		}

		if (!oldpulse && pulse_stable)
			n_pulses++;

		oldidle = idle_stable;
		oldpulse = pulse_stable;

		
		if (button_release_counter < 255)
			button_release_counter++;	// increase release counter
			
		
		
		if (usbInterruptIsReady() && state != STATE_WAIT && LED_state != 0xff)
		{
			switch (state)
			{
				case STATE_SEND_KEY:
					buildReport(key_to_send);
					state = STATE_RELEASE_KEY;	// release next
					PORTC &= ~LED_BLUE;
					break;
				
				case STATE_RELEASE_KEY:
					buildReport(-1);
					state = STATE_WAIT;	// go back to waiting
					PORTC |= LED_BLUE;
					break;
				
				default:
					state = STATE_WAIT;	// should not happen
			}

			// start sending
			usbSetInterrupt((void *) &keyboard_report, sizeof(keyboard_report));
		}
	}

	jump_to_bootloader();
	return 0;
}
