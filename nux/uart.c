/*************************************************************************
Copyright (C) 2011  busware

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*************************************************************************/
#include <stdarg.h>

/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/sysctl.h"
#include "driverlib/uart.h"
#include "driverlib/debug.h"

#include "modules.h"

//*****************************************************************************
//
// The error routine that is called if the driver library encounters an error.
//
//*****************************************************************************
#ifdef DEBUG
void
__error__(char *pcFilename, unsigned long ulLine)
{
}
#endif

//*****************************************************************************
//
// A mapping from an integer between 0 and 15 to its ASCII character
// equivalent.
//
//*****************************************************************************
static const char * const g_pcHex = "0123456789abcdef";

static const unsigned long periph_uart[3] = {
	SYSCTL_PERIPH_UART0, SYSCTL_PERIPH_UART1, SYSCTL_PERIPH_UART2
};

static const unsigned long periph_gpio[3] = {
	SYSCTL_PERIPH_GPIOA, SYSCTL_PERIPH_GPIOD, SYSCTL_PERIPH_GPIOG
};

static const unsigned long gpio_port[3] = {
	GPIO_PORTA_BASE, GPIO_PORTD_BASE, GPIO_PORTG_BASE
};

static const unsigned long gpio_pins[3] = {
	GPIO_PIN_0 | GPIO_PIN_1, GPIO_PIN_2 | GPIO_PIN_3, GPIO_PIN_0 | GPIO_PIN_1
};

static const unsigned long uart_base[3] = {
	UART0_BASE, UART1_BASE, UART2_BASE
};

//*****************************************************************************
//
// Send a string to the UART.
//
//*****************************************************************************
void UARTSend(unsigned long ulBase, const char *pucBuffer, unsigned short ulCount) {
	unsigned short i;
	
    for(i = 0; i < ulCount; i++) {
       	UARTCharPut(ulBase, pucBuffer[i]); // Send the character to the UART output.
	}
}

/*
	UART interrupt handler.
	Caution: As long as this function uses a function ending with 'FromISR' the priority must set between 0xC0 and 0xA0
	
*/
void generic_uart_handler(struct uart_info *uart) {
	unsigned long ulStatus;
	portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;
	char data;

	ulStatus = UARTIntStatus(uart->base, true);
	// Clear the asserted interrupts.
	UARTIntClear(uart->base, ulStatus);

	if( ulStatus & (UART_INT_RX | UART_INT_RT) )     {
		while (UARTCharsAvail(uart->base)) {
			data = UARTCharGetNonBlocking(uart->base);
			uart->recv++;
			if(errQUEUE_FULL == xQueueSendFromISR( uart->queue, &data, &xHigherPriorityTaskWoken )) {
				uart->lost++;
			}
		}
	} else if(ulStatus & (UART_INT_OE | UART_INT_BE | UART_INT_PE | UART_INT_FE)) {
		uart->err++;
	}
	portEND_SWITCHING_ISR(xHigherPriorityTaskWoken);
}

void UART0IntHandler(void) {
}

void UART1IntHandler(void) {
	generic_uart_handler(get_uart_profile(MODULE1));
}

void UART2IntHandler(void) {
	generic_uart_handler(get_uart_profile(MODULE2));
}

typedef void (*inthandler)();
static const inthandler handler[3] = {
	UART0IntHandler,UART1IntHandler,UART2IntHandler
};

static unsigned long interrupts[3] = {
	INT_UART0,INT_UART1,INT_UART2
};

void uart_init(unsigned short uart_idx, unsigned long baud, unsigned short config) {
	SysCtlPeripheralEnable(periph_uart[uart_idx]);
	SysCtlPeripheralEnable(periph_gpio[uart_idx]);
	
	GPIOPinTypeUART(gpio_port[uart_idx], gpio_pins[uart_idx]);

	UARTConfigSetExpClk(uart_base[uart_idx], SysCtlClockGet(), baud, config);
	switch(uart_idx) {
		case 1:
		case 2: {
			UARTFIFOEnable(uart_base[uart_idx]);
			UARTFIFOLevelSet(uart_base[uart_idx], UART_FIFO_TX7_8, UART_FIFO_RX7_8);
			UARTIntRegister(uart_base[uart_idx], handler[uart_idx]);
			// Set interrupt priority to number higher than configMAX_SYSCALL_INTERRUPT_PRIORITY
			// defined in FreeRTOSConfig.h, see www.freertos.org
			IntPrioritySet(interrupts[uart_idx], SET_SYSCALL_INTERRUPT_PRIORITY(6));
			IntEnable(interrupts[uart_idx]);
		    UARTIntEnable(uart_base[uart_idx], UART_INT_RX | UART_INT_RT | UART_INT_OE);
			break;
		}
	}

}


//*****************************************************************************
//
//! A simple UART based get string function, with some line processing.
//!
//! \param pcBuf points to a buffer for the incoming string from the UART.
//! \param ulLen is the length of the buffer for storage of the string,
//! including the trailing 0.
//!
//! This function will receive a string from the UART input and store the
//! characters in the buffer pointed to by \e pcBuf.  The characters will
//! continue to be stored until a termination character is received.  The
//! termination characters are CR, LF, or ESC.  A CRLF pair is treated as a
//! single termination character.  The termination characters are not stored in
//! the string.  The string will be terminated with a 0 and the function will
//! return.
//!
//! In both buffered and unbuffered modes, this function will block until
//! a termination character is received.  If non-blocking operation is required
//! in buffered mode, a call to UARTPeek() may be made to determine whether
//! a termination character already exists in the receive buffer prior to
//! calling UARTgets().
//!
//! Since the string will be null terminated, the user must ensure that the
//! buffer is sized to allow for the additional null character.
//!
//! \return Returns the count of characters that were stored, not including
//! the trailing 0.
//
//*****************************************************************************
int UARTgets(unsigned long ulBase, char *pcBuf, unsigned long ulLen) {
	unsigned long ulCount = 0;
	char cChar;
	static char bLastWasCR = 0;

	//
	// Check the arguments.
	//
	ASSERT(pcBuf != 0);
	ASSERT(ulLen != 0);
	ASSERT(ulBase != 0);

	//
	// Adjust the length back by 1 to leave space for the trailing
	// null terminator.
	//
	ulLen--;

	//
	// Process characters until a newline is received.
	//
	while(1)  {
		cChar = UARTCharGet(ulBase);

		//
		// See if the backspace key was pressed.
		//
		if(cChar == '\b') {
			//
			// If there are any characters already in the buffer, then delete
			// the last.
			//
			if(ulCount) {
				//
				// Rub out the previous character.
				//
				UARTSend(ulBase,"\b \b", 3);
				ulCount--;
			}

			//
			// Skip ahead to read the next character.
			//
			continue;
		}

		//
		// If this character is LF and last was CR, then just gobble up the
		// character because the EOL processing was taken care of with the CR.
		//
		if((cChar == '\n') && bLastWasCR) {
			bLastWasCR = 0;
			continue;
		}

		//
		// See if a newline or escape character was received.
		//
		if((cChar == '\r') || (cChar == '\n') || (cChar == 0x1b)) {
			//
			// If the character is a CR, then it may be followed by a LF which
			// should be paired with the CR.  So remember that a CR was
			// received.
			//
			if(cChar == '\r') {
				bLastWasCR = 1;
			}
			break;
		}

		//
		// Process the received character as long as we are not at the end of
		// the buffer.  If the end of the buffer has been reached then all
		// additional characters are ignored until a newline is received.
		//
		if(ulCount < ulLen) {
			pcBuf[ulCount] = cChar;
			ulCount++;

			//
			// Reflect the character back to the user.
			//
			UARTCharPut(ulBase, cChar);
		}
	}

	//
	// Add a null termination to the string.
	//
	pcBuf[ulCount] = 0;

	//
	// Send a CRLF pair to the terminal to end the line.
	//
	UARTSend(ulBase,"\r\n", 2);

	return(ulCount);
}
