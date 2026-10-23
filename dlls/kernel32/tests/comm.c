/* Unit test suite for comm functions
 *
 * Copyright 2003 Kevin Groeneveld
 * Copyright 2005 Uwe Bonnes
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "wine/test.h"
#include "winternl.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winuser.h"
#include "dbt.h"

#define TIMEOUT 1000   /* one second for Timeouts*/
#define SLOWBAUD 150
#define FASTBAUD 115200
#define TIMEDELTA 150  /* 150 ms uncertainty allowed */

/* Define the appropriate LOOPBACK(s) TRUE if you have a Loopback cable with
 * the mentioned shorts connected to your Serial port
 */
#define LOOPBACK_TXD_RXD  FALSE /* Sub-D 9: Short 2-3 */
#define LOOPBACK_CTS_RTS  FALSE /* Sub-D 9: Short 7-8 */
#define LOOPBACK_DTR_DSR  FALSE /* Sub-D 9: Short 4-6 */
#define LOOPBACK_DTR_RING FALSE /* Sub-D 9: Short 4-9 */
#define LOOPBACK_DTR_DCD  FALSE /* Sub-D 9: Short 4-1 */
/* Many Linux serial drivers have the TIOCM_LOOP flag in the TIOCM_SET ioctl
 * available. For the 8250 this is equivalent to TXD->RXD, OUT2->DCD,
 * OUT1->RI, RTS->CTS and DTR->DSR
 */
/* use variables and not #define to compile the code */
static BOOL loopback_txd_rxd  = LOOPBACK_TXD_RXD;
static BOOL loopback_rts_cts  = LOOPBACK_CTS_RTS;
static BOOL loopback_dtr_dsr  = LOOPBACK_DTR_DSR;
static BOOL loopback_dtr_ring = LOOPBACK_DTR_RING;
static BOOL loopback_dtr_dcd  = LOOPBACK_DTR_DCD;

static NTSTATUS (WINAPI *pNtReadFile)(HANDLE hFile, HANDLE hEvent,
                                      PIO_APC_ROUTINE apc, void* apc_user,
                                      PIO_STATUS_BLOCK io_status, void* buffer, ULONG length,
                                      PLARGE_INTEGER offset, PULONG key);
static NTSTATUS (WINAPI *pNtWriteFile)(HANDLE hFile, HANDLE hEvent,
                                       PIO_APC_ROUTINE apc, void* apc_user,
                                       PIO_STATUS_BLOCK io_status,
                                       const void* buffer, ULONG length,
                                       PLARGE_INTEGER offset, PULONG key);

typedef struct
{
	char string[100];
	BOOL result;
	BOOL old_style;
	DCB dcb1, dcb2;
	COMMTIMEOUTS timeouts1, timeouts2;
} TEST;

static const TEST test[] =
{
	{
		"baud=9600 parity=e data=5 stop=1 xon=on odsr=off octs=off dtr=on rts=on idsr=on",
		TRUE, FALSE,
		{ 0x00000000, 0x00002580, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x05, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00002580, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x05, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"baud=0 parity=M data=6 stop=1.5 xon=off odsr=on octs=ON dtr=off rts=off idsr=OFF",
		TRUE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x06, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00000000, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x06, 0x03, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"BAUD=4000000000 parity=n data=7 stop=2 to=off",
		TRUE, FALSE,
		{ 0x00000000, 0xee6b2800, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x07, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xee6b2800, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x07, 0x00, 0x02, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 }
	},
	{
		"Baud=115200 Parity=O Data=8 To=On",
		TRUE, FALSE,
		{ 0x00000000, 0x0001c200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x0001c200, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x08, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0000EA60 },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0000EA60 }
	},
	{
		"PaRiTy=s           Data=7          DTR=on",
		TRUE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x07, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x07, 0x04, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"data=4",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"data=9",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"parity=no",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"stop=0",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"stop=1.501",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"stop=3",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"to=foobar",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		" baud=9600",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"baud= 9600",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"baud=9600,data=8",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"11,n,8,1",
		TRUE, TRUE,
		{ 0x00000000, 0x0000006e, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x0000006e, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x08, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"30 ,E, 5,1.5",
		TRUE, TRUE,
		{ 0x00000000, 0x0000012c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x05, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x0000012c, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x05, 0x02, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"60, m, 6, 2 ",
		TRUE, TRUE,
		{ 0x00000000, 0x00000258, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x06, 0x03, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00000258, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x06, 0x03, 0x02, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"12 , o , 7 , 1",
		TRUE, TRUE,
		{ 0x00000000, 0x000004b0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x000004b0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x07, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"24,s,8,1.5",
		TRUE, TRUE,
		{ 0x00000000, 0x00000960, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x08, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00000960, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x08, 0x04, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"48,n,8,1,p",
		TRUE, TRUE,
		{ 0x00000000, 0x000012c0, 0, 0, 1, 1, 2, 0, 0, 0, 0, 0, 0, 2, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x000012c0, 1, 1, 1, 1, 2, 1, 1, 0, 0, 1, 1, 2, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x08, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"96,N,8,1 , x ",
		TRUE, TRUE,
		{ 0x00000000, 0x00002580, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00002580, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x08, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"19, e, 7, 1, x",
		TRUE, TRUE,
		{ 0x00000000, 0x00004b00, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x07, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00004b00, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x07, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"0,M,7,1,P",
		TRUE, TRUE,
		{ 0x00000000, 0x00000000, 0, 0, 1, 1, 2, 0, 0, 0, 0, 0, 0, 2, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00000000, 1, 1, 1, 1, 2, 1, 1, 0, 0, 1, 1, 2, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x07, 0x03, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"4000000000,O,7,1.5,X",
		TRUE, TRUE,
		{ 0x00000000, 0xee6b2800, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x07, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xee6b2800, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x07, 0x01, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"96,N,8,1 to=on",
		FALSE, TRUE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"96,NO,8,1",
		FALSE, TRUE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"96,N,4,1",
		FALSE, TRUE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"96,N,9,1",
		FALSE, TRUE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"96,N,8,0",
		FALSE, TRUE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"96,N,8,3",
		FALSE, TRUE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"96,N,8,1,K",
		FALSE, TRUE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"COM0:baud=115200",
		FALSE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"COMx:baud=38400 data=8",
		TRUE, FALSE,
		{ 0x00000000, 0x00009600, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00009600, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x08, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"COMx  :to=on stop=1.5",
		TRUE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0000EA60 },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0000EA60 }
	},
	{
		"COMx:               baud=12345     data=7",
		TRUE, FALSE,
		{ 0x00000000, 0x00003039, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00003039, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x07, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"COMx : xon=on odsr=off",
		TRUE, FALSE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 0, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"COM0:9600,N,8,1",
		FALSE, TRUE,
		{ 0x00000000, 0x00000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0xffffffff, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"COMx:9600,N,8,1",
		TRUE, TRUE,
		{ 0x00000000, 0x00002580, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00002580, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x08, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"COMx:  11,E,7,2",
		TRUE, TRUE,
		{ 0x00000000, 0x0000006e, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x07, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x0000006e, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x07, 0x02, 0x02, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"COMx  :19,M,5,1",
		TRUE, TRUE,
		{ 0x00000000, 0x00004b00, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x05, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x00004b00, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x05, 0x03, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
	{
		"COMx  :    57600,S,6,2,x",
		TRUE, TRUE,
		{ 0x00000000, 0x0000e100, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0x00000, 0x0000, 0x0000, 0x0000, 0x06, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0000 },
		{ 0xffffffff, 0x0000e100, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0x1ffff, 0xffff, 0xffff, 0xffff, 0x06, 0x04, 0x02, 0xff, 0xff, 0xff, 0xff, 0xff, 0xffff },
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff }
	},
};

/* This function can be useful if you are modifying the test cases and want to
   output the contents of a DCB structure. */
/*static print_dcb(DCB *pdcb)
{
	printf("0x%08x, 0x%08x, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, 0x%05x, 0x%04x, 0x%04x, 0x%04x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%04x\n",
		pdcb->DCBlength,
		pdcb->BaudRate,
		pdcb->fBinary,
		pdcb->fParity,
		pdcb->fOutxCtsFlow,
		pdcb->fOutxDsrFlow,
		pdcb->fDtrControl,
		pdcb->fDsrSensitivity,
		pdcb->fTXContinueOnXoff,
		pdcb->fOutX,
		pdcb->fInX,
		pdcb->fErrorChar,
		pdcb->fNull,
		pdcb->fRtsControl,
		pdcb->fAbortOnError,
		pdcb->fDummy2,
		pdcb->wReserved,
		pdcb->XonLim,
		pdcb->XoffLim,
		pdcb->ByteSize,
		pdcb->Parity,
		pdcb->StopBits,
		pdcb->XonChar & 0xff,
		pdcb->XoffChar & 0xff,
		pdcb->ErrorChar & 0xff,
		pdcb->EofChar & 0xff,
		pdcb->EvtChar & 0xff,
		pdcb->wReserved1 & 0xffff );
} */

static void check_result(const char *function, const TEST *ptest, int initial_value, BOOL result)
{
	DWORD LastError = GetLastError();
	DWORD CorrectError = (ptest->result ? 0xdeadbeef : ERROR_INVALID_PARAMETER);

	ok(LastError == CorrectError, "%s(\"%s\"), 0x%02x: GetLastError() returned %ld, should be %ld\n", function, ptest->string, initial_value, LastError, CorrectError);
	ok(result == ptest->result, "%s(\"%s\"), 0x%02x: return value should be %s\n", function, ptest->string, initial_value, ptest->result ? "TRUE" : "FALSE");
}

#define check_dcb_member(a,b) ok(pdcb1->a == pdcb2->a, "%s(\"%s\"), 0x%02x: "#a" is "b", should be "b"\n", function, ptest->string, initial_value, pdcb1->a, pdcb2->a)
#define check_dcb_member2(a,c,b) if(pdcb2->a == c) { check_dcb_member(a,b); } else { ok(pdcb1->a == pdcb2->a || pdcb1->a == c, "%s(\"%s\"), 0x%02x: "#a" is "b", should be "b" or "b"\n", function, ptest->string, initial_value, pdcb1->a, pdcb2->a, c); }

static void check_dcb(const char *function, const TEST *ptest, int initial_value, const DCB *pdcb1, const DCB *pdcb2)
{
	/* DCBlength is a special case since Win 9x sets it but NT does not.
	   We will accept either as correct. */
	check_dcb_member2(DCBlength, (DWORD)sizeof(DCB), "%lu");

	/* For old style control strings Win 9x does not set the next five members, NT does. */
	if(ptest->old_style && ptest->result)
	{
		check_dcb_member2(fOutxCtsFlow, ((unsigned int)initial_value & 1), "%u");
		check_dcb_member2(fDtrControl, ((unsigned int)initial_value & 3), "%u");
		check_dcb_member2(fOutX, ((unsigned int)initial_value & 1), "%u");
		check_dcb_member2(fInX, ((unsigned)initial_value & 1), "%u");
		check_dcb_member2(fRtsControl, ((unsigned)initial_value & 3), "%u");
	}
	else
	{
		check_dcb_member(fOutxCtsFlow, "%u");
		check_dcb_member(fDtrControl, "%u");
		check_dcb_member(fOutX, "%u");
		check_dcb_member(fInX, "%u");
		check_dcb_member(fRtsControl, "%u");
	}

	if(ptest->result)
	{
		/* For the idsr=xxx parameter, NT sets fDsrSensitivity, 9x sets
		   fOutxDsrFlow. */
		if(!ptest->old_style)
		{
			check_dcb_member2(fOutxDsrFlow, pdcb2->fDsrSensitivity, "%u");
			check_dcb_member2(fDsrSensitivity, pdcb2->fOutxDsrFlow, "%u");
		}
		else
		{
			/* For old style control strings Win 9x does not set the
			   fOutxDsrFlow member, NT does. */
			check_dcb_member2(fOutxDsrFlow, ((unsigned int)initial_value & 1), "%u");
			check_dcb_member(fDsrSensitivity, "%u");
		}
	}
	else
	{
		check_dcb_member(fOutxDsrFlow, "%u");
		check_dcb_member(fDsrSensitivity, "%u");
	}

	/* Check the result of the DCB members. */
	check_dcb_member(BaudRate, "%lu");
	check_dcb_member(fBinary, "%u");
	check_dcb_member(fParity, "%u");
	check_dcb_member(fTXContinueOnXoff, "%u");
	check_dcb_member(fErrorChar, "%u");
	check_dcb_member(fNull, "%u");
	check_dcb_member(fAbortOnError, "%u");
	check_dcb_member(fDummy2, "%u");
	check_dcb_member(wReserved, "%u");
	check_dcb_member(XonLim, "%u");
	check_dcb_member(XoffLim, "%u");
	check_dcb_member(ByteSize, "%u");
	check_dcb_member(Parity, "%u");
	check_dcb_member(StopBits, "%u");
	check_dcb_member(XonChar, "%d");
	check_dcb_member(XoffChar, "%d");
	check_dcb_member(ErrorChar, "%d");
	check_dcb_member(EofChar, "%d");
	check_dcb_member(EvtChar, "%d");
	check_dcb_member(wReserved1, "%u");
}

#define check_timeouts_member(a) ok(ptimeouts1->a == ptimeouts2->a, "%s(\"%s\"), 0x%02x: "#a" is %lu, should be %lu\n", function, ptest->string, initial_value, ptimeouts1->a, ptimeouts2->a);

static void check_timeouts(const char *function, const TEST *ptest, int initial_value, const COMMTIMEOUTS *ptimeouts1, const COMMTIMEOUTS *ptimeouts2)
{
	check_timeouts_member(ReadIntervalTimeout);
	check_timeouts_member(ReadTotalTimeoutMultiplier);
	check_timeouts_member(ReadTotalTimeoutConstant);
	check_timeouts_member(WriteTotalTimeoutMultiplier);
	check_timeouts_member(WriteTotalTimeoutConstant);
}

static void test_BuildCommDCBA(const char *string, const TEST *ptest, int initial_value, const DCB *pexpected_dcb)
{
	BOOL result;
	DCB dcb;

	/* set initial conditions */
	memset(&dcb, initial_value, sizeof(DCB));
	SetLastError(0xdeadbeef);

	result = BuildCommDCBA(string, &dcb);

	/* check results */
	check_result("BuildCommDCBA", ptest, initial_value, result);
	check_dcb("BuildCommDCBA", ptest, initial_value, &dcb, pexpected_dcb);
}

static void test_BuildCommDCBAndTimeoutsA(const char *string, const TEST *ptest, int initial_value, const DCB *pexpected_dcb, const COMMTIMEOUTS *pexpected_timeouts)
{
	BOOL result;
	DCB dcb;
	COMMTIMEOUTS timeouts;

	/* set initial conditions */
	memset(&dcb, initial_value, sizeof(DCB));
	memset(&timeouts, initial_value, sizeof(COMMTIMEOUTS));
	SetLastError(0xdeadbeef);

	result = BuildCommDCBAndTimeoutsA(string, &dcb, &timeouts);

	/* check results */
	check_result("BuildCommDCBAndTimeoutsA", ptest, initial_value, result);
	check_dcb("BuildCommDCBAndTimeoutsA", ptest, initial_value, &dcb, pexpected_dcb);
	check_timeouts("BuildCommDCBAndTimeoutsA", ptest, initial_value, &timeouts, pexpected_timeouts);
}

static void test_BuildCommDCBW(const char *string, const TEST *ptest, int initial_value, const DCB *pexpected_dcb)
{
	BOOL result;
	DCB dcb;
	WCHAR wide_string[sizeof(ptest->string)];
	static int reportedDCBW = 0;

	MultiByteToWideChar(CP_ACP, 0, string, -1, wide_string, ARRAY_SIZE(wide_string));

	/* set initial conditions */
	memset(&dcb, initial_value, sizeof(DCB));
	SetLastError(0xdeadbeef);

	result = BuildCommDCBW(wide_string, &dcb);

	if(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED)
	{
		if(!reportedDCBW++)
			win_skip("BuildCommDCBW is not implemented\n");
		return;
	}

	/* check results */
	check_result("BuildCommDCBW", ptest, initial_value, result);
	check_dcb("BuildCommDCBW", ptest, initial_value, &dcb, pexpected_dcb);
}

static void test_BuildCommDCBAndTimeoutsW(const char *string, const TEST *ptest, int initial_value, const DCB *pexpected_dcb, const COMMTIMEOUTS *pexpected_timeouts)
{
	BOOL result;
	DCB dcb;
	COMMTIMEOUTS timeouts;
	WCHAR wide_string[sizeof(ptest->string)];
	static int reportedDCBAndTW = 0;

	MultiByteToWideChar(CP_ACP, 0, string, -1, wide_string, ARRAY_SIZE(wide_string));

	/* set initial conditions */
	memset(&dcb, initial_value, sizeof(DCB));
	memset(&timeouts, initial_value, sizeof(COMMTIMEOUTS));
	SetLastError(0xdeadbeef);

	result = BuildCommDCBAndTimeoutsW(wide_string, &dcb, &timeouts);

	if(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED)
	{
		if(!reportedDCBAndTW++)
			win_skip("BuildCommDCBAndTimeoutsW is not implemented\n");
		return;
	}

	/* check results */
	check_result("BuildCommDCBAndTimeoutsW", ptest, initial_value, result);
	check_dcb("BuildCommDCBAndTimeoutsW", ptest, initial_value, &dcb, pexpected_dcb);
	check_timeouts("BuildCommDCBAndTimeoutsW", ptest, initial_value, &timeouts, pexpected_timeouts);
}

static void test_BuildCommDCB(void)
{
	char port_name[] = "COMx";
	char port = 0;
	unsigned int i;
	char *ptr;

	/* Some of these tests require a valid COM port.  This loop will try to find
	   a valid port. */
	for(port_name[3] = '1'; port_name[3] <= '9'; port_name[3]++)
	{
		COMMCONFIG commconfig;
		DWORD size = sizeof(COMMCONFIG);

		if(GetDefaultCommConfigA(port_name, &commconfig, &size))
		{
			port = port_name[3];
			break;
		}
	}

	if(!port)
		trace("Could not find a valid COM port.  Some tests will be skipped.\n");

        for(i = 0; i < ARRAY_SIZE(test); i++)
	{
                char string[sizeof(test[i].string)];

                strcpy(string, test[i].string);

		/* Check if this test case needs a valid COM port. */
		ptr = strstr(string, "COMx");

		/* If required, substitute valid port number into device control string. */
		if(ptr)
		{
			if(port)
				ptr[3] = port;
			else
				continue;
		}

		test_BuildCommDCBA(string, &test[i], 0x00, &test[i].dcb1);
		test_BuildCommDCBA(string, &test[i], 0xff, &test[i].dcb2);
		test_BuildCommDCBAndTimeoutsA(string, &test[i], 0x00, &test[i].dcb1, &test[i].timeouts1);
		test_BuildCommDCBAndTimeoutsA(string, &test[i], 0xff, &test[i].dcb2, &test[i].timeouts2);

		test_BuildCommDCBW(string, &test[i], 0x00, &test[i].dcb1);
		test_BuildCommDCBW(string, &test[i], 0xff, &test[i].dcb2);
		test_BuildCommDCBAndTimeoutsW(string, &test[i], 0x00, &test[i].dcb1, &test[i].timeouts1);
		test_BuildCommDCBAndTimeoutsW(string, &test[i], 0xff, &test[i].dcb2, &test[i].timeouts2);
	}
}

static HANDLE test_OpenComm(BOOL doOverlap)
{
    HANDLE hcom = INVALID_HANDLE_VALUE;
    char port_name[] = "COMx";
    static BOOL shown = FALSE;
    DWORD errors;
    COMSTAT comstat;

    /* Try to find a port */
    for(port_name[3] = '1'; port_name[3] <= '9'; port_name[3]++)
    {
	hcom = CreateFileA( port_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
			   (doOverlap)?FILE_FLAG_OVERLAPPED:0, NULL );
	if (hcom != INVALID_HANDLE_VALUE)
	    break;
    }
    if(!shown)
    {
	if (hcom == INVALID_HANDLE_VALUE)
	    trace("Could not find a valid COM port.\n");
	else
	    trace("Found Com port %s. Connected devices may disturb results\n", port_name);
	/*shown = TRUE; */
    }
    if (hcom != INVALID_HANDLE_VALUE)
    {
        BOOL ret;

        ret = ClearCommError(hcom, &errors, &comstat);
        if (!ret && (GetLastError() == ERROR_NOT_READY || GetLastError() == ERROR_INVALID_HANDLE))
        {
            if (GetLastError() == ERROR_NOT_READY)
                trace("%s doesn't respond, skipping the test\n", port_name);
            else
                trace("%s is not a real serial port, skipping the test\n", port_name);
            CloseHandle(hcom);
            return INVALID_HANDLE_VALUE;
        }

        ok(ret, "Unexpected error %lu on open\n", GetLastError());
        ok(comstat.cbInQue == 0, "Unexpected %ld chars in InQueue\n",comstat.cbInQue);
        ok(comstat.cbOutQue == 0, "Still pending %ld characters in OutQueue\n", comstat.cbOutQue);
        ok(errors == 0, "Unexpected errors 0x%08lx\n", errors);
    }
    return hcom;
}

static void test_GetModemStatus(HANDLE hcom)
{
    DWORD ModemStat = 0;

    ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
    trace("GetCommModemStatus returned 0x%08lx->%s%s%s%s\n", ModemStat,
	  (ModemStat &MS_RLSD_ON)?"MS_RLSD_ON ":"",
	  (ModemStat &MS_RING_ON)?"MS_RING_ON ":"",
	  (ModemStat &MS_DSR_ON)?"MS_DSR_ON ":"",
	  (ModemStat &MS_CTS_ON)?"MS_CTS_ON ":"");
}

/* When we don't write anything, Read should time out even on a loopbacked port */
static void test_ReadTimeOut(void)
{
    HANDLE hcom;
    DCB dcb;
    COMMTIMEOUTS timeouts;
    char rbuf[32];
    DWORD before, after, read, timediff, LastError;
    BOOL res;

    hcom = test_OpenComm(FALSE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    test_GetModemStatus(hcom);

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    dcb.BaudRate = FASTBAUD;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.fRtsControl=RTS_CONTROL_ENABLE;
    dcb.fDtrControl=DTR_CONTROL_ENABLE;
    dcb.StopBits = ONESTOPBIT;
    ok(SetCommState(hcom, &dcb), "SetCommState failed\n");

    ZeroMemory( &timeouts, sizeof(timeouts));
    timeouts.ReadTotalTimeoutConstant = TIMEOUT;
    ok(SetCommTimeouts(hcom, &timeouts),"SetCommTimeouts failed\n");

    before = GetTickCount();
    SetLastError(0xdeadbeef);
    res = ReadFile(hcom, rbuf, sizeof(rbuf), &read, NULL);
    LastError = GetLastError();
    after = GetTickCount();
    ok( res == TRUE, "A timed-out read should return TRUE\n");
    ok( LastError == 0xdeadbeef, "err=%ld\n", LastError);
    timediff = after - before;
    ok( timediff > TIMEOUT>>2 && timediff < TIMEOUT *2,
	"Unexpected TimeOut %ld, expected %d\n", timediff, TIMEOUT);

    CloseHandle(hcom);
}

static void test_waittxempty(void)
{
    HANDLE hcom;
    DCB dcb;
    COMMTIMEOUTS timeouts;
    char tbuf[]="test_waittxempty";
    DWORD before, after, bytes, timediff, evtmask, errors, i;
    BOOL res;
    DWORD baud = SLOWBAUD;
    OVERLAPPED ovl_write, ovl_wait, ovl_wait2;
    COMSTAT stat;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    /* set a low baud rate to have ample time*/
    res = GetCommState(hcom, &dcb);
    ok(res, "GetCommState error %ld\n", GetLastError());
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.fRtsControl=RTS_CONTROL_ENABLE;
    dcb.fDtrControl=DTR_CONTROL_ENABLE;
    dcb.StopBits = ONESTOPBIT;
    res = SetCommState(hcom, &dcb);
    ok(res, "SetCommState error %ld\n", GetLastError());

    ZeroMemory( &timeouts, sizeof(timeouts));
    timeouts.ReadTotalTimeoutConstant = TIMEOUT;
    res = SetCommTimeouts(hcom, &timeouts);
    ok(res,"SetCommTimeouts error %ld\n", GetLastError());

    res = SetupComm(hcom, 1024, 1024);
    ok(res, "SetUpComm error %ld\n", GetLastError());

    /* calling SetCommMask after WriteFile leads to WaitCommEvent failures
     * due to timeout (no events) under testbot VMs and VirtualBox
     */
    res = SetCommMask(hcom, EV_TXEMPTY);
    ok(res, "SetCommMask error %ld\n", GetLastError());

    SetLastError(0xdeadbeef);
    res = WriteFile(hcom, tbuf, sizeof(tbuf), &bytes, NULL);
    ok(!res, "WriteFile on an overlapped handle without ovl structure should fail\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %ld\n", GetLastError());

    ovl_write.Offset = 0;
    ovl_write.OffsetHigh = 0;
    ovl_write.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    before = GetTickCount();
    SetLastError(0xdeadbeef);
    res = WriteFile(hcom, tbuf, sizeof(tbuf), &bytes, &ovl_write);
    after = GetTickCount();
    ok((!res && GetLastError() == ERROR_IO_PENDING) || (res && bytes == sizeof(tbuf)),
       "WriteFile returned %d, written %lu bytes, error %ld\n", res, bytes, GetLastError());
    if (!res) ok(!bytes, "expected 0, got %lu\n", bytes);
    ok(after - before < 30, "WriteFile took %ld ms to write %ld Bytes at %ld Baud\n",
       after - before, bytes, baud);
    /* don't wait for WriteFile completion */

    ovl_wait.Offset = 0;
    ovl_wait.OffsetHigh = 0;
    ovl_wait.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    evtmask = 0;
    before = GetTickCount();
    SetLastError(0xdeadbeef);
    res = WaitCommEvent(hcom, &evtmask, &ovl_wait);
    ok(res || GetLastError() == ERROR_IO_PENDING, "WaitCommEvent error %ld\n", GetLastError());
    after = GetTickCount();
    ok(after - before < 30, "WaitCommEvent should have returned immediately, took %ld ms\n", after - before);
    res = WaitForSingleObject(ovl_wait.hEvent, 1500);
    ok(res == WAIT_OBJECT_0, "WaitCommEvent failed with a timeout\n");
    if (res == WAIT_OBJECT_0)
    {
        res = GetOverlappedResult(hcom, &ovl_wait, &bytes, FALSE);
        ok(res, "GetOverlappedResult reported error %ld\n", GetLastError());
        ok(bytes == sizeof(evtmask), "expected %u, written %lu\n", (UINT)sizeof(evtmask), bytes);
        res = TRUE;
    }
    else
    {
        /* unblock pending wait */
        trace("recovering after WAIT_TIMEOUT...\n");
        res = SetCommMask(hcom, EV_TXEMPTY);
        ok(res, "SetCommMask error %ld\n", GetLastError());

        res = WaitForSingleObject(ovl_wait.hEvent, TIMEOUT);
        ok(res == WAIT_OBJECT_0, "WaitCommEvent failed with a timeout\n");

        res = FALSE;
    }
    after = GetTickCount();
    ok(res, "WaitCommEvent error %ld\n", GetLastError());
    ok(evtmask & EV_TXEMPTY, "WaitCommEvent: expected EV_TXEMPTY, got %#lx\n", evtmask);
    CloseHandle(ovl_wait.hEvent);

    timediff = after - before;
    trace("WaitCommEvent for EV_TXEMPTY took %ld ms (timeout 1500)\n", timediff);
    ok(timediff < 1200, "WaitCommEvent used %ld ms for waiting\n", timediff);

    res = WaitForSingleObject(ovl_write.hEvent, 0);
    ok(res == WAIT_OBJECT_0, "WriteFile failed with a timeout\n");
    res = GetOverlappedResult(hcom, &ovl_write, &bytes, FALSE);
    ok(res, "GetOverlappedResult reported error %ld\n", GetLastError());
    ok(bytes == sizeof(tbuf), "expected %u, written %lu\n", (UINT)sizeof(tbuf), bytes);
    CloseHandle(ovl_write.hEvent);

    CloseHandle(hcom);

    for (i = 0; i < 2; i++)
    {
        hcom = test_OpenComm(TRUE);
        if (hcom == INVALID_HANDLE_VALUE) return;

        res = SetCommMask(hcom, EV_TXEMPTY);
        ok(res, "SetCommMask error %ld\n", GetLastError());

        if (i == 0)
        {
            ovl_write.Offset = 0;
            ovl_write.OffsetHigh = 0;
            ovl_write.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            SetLastError(0xdeadbeef);
            res = WriteFile(hcom, tbuf, sizeof(tbuf), &bytes, &ovl_write);
            ok((!res && GetLastError() == ERROR_IO_PENDING) || (res && bytes == sizeof(tbuf)),
               "WriteFile returned %d, written %lu bytes, error %ld\n", res, bytes, GetLastError());
            if (!res) ok(!bytes, "expected 0, got %lu\n", bytes);

            ClearCommError(hcom, &errors, &stat);
            ok(stat.cbInQue == 0, "InQueue should be empty, got %ld bytes\n", stat.cbInQue);
            ok(stat.cbOutQue != 0 || broken(stat.cbOutQue == 0) /* VM */, "OutQueue should not be empty\n");
            ok(errors == 0, "ClearCommErrors: Unexpected error 0x%08lx\n", errors);

            res = GetOverlappedResult(hcom, &ovl_write, &bytes, TRUE);
            ok(res, "GetOverlappedResult reported error %ld\n", GetLastError());
            ok(bytes == sizeof(tbuf), "expected %u, written %lu\n", (UINT)sizeof(tbuf), bytes);
            CloseHandle(ovl_write.hEvent);

            res = FlushFileBuffers(hcom);
            ok(res, "FlushFileBuffers error %ld\n", GetLastError());
        }

        ClearCommError(hcom, &errors, &stat);
        ok(stat.cbInQue == 0, "InQueue should be empty, got %ld bytes\n", stat.cbInQue);
        ok(stat.cbOutQue == 0, "OutQueue should be empty, got %ld bytes\n", stat.cbOutQue);
        ok(errors == 0, "ClearCommErrors: Unexpected error 0x%08lx\n", errors);

        ovl_wait.Offset = 0;
        ovl_wait.OffsetHigh = 0;
        ovl_wait.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        evtmask = 0;
        SetLastError(0xdeadbeef);
        res = WaitCommEvent(hcom, &evtmask, &ovl_wait);
        ok(res /* busy system */ || GetLastError() == ERROR_IO_PENDING,
           "%ld: WaitCommEvent error %ld\n", i, GetLastError());

        res = WaitForSingleObject(ovl_wait.hEvent, TIMEOUT);
        if (i == 0)
            ok(res == WAIT_OBJECT_0, "WaitCommEvent failed with a timeout\n");
        else
            ok(res == WAIT_TIMEOUT, "WaitCommEvent should fail with a timeout\n");
        if (res == WAIT_OBJECT_0)
        {
            res = GetOverlappedResult(hcom, &ovl_wait, &bytes, FALSE);
            ok(res, "GetOverlappedResult reported error %ld\n", GetLastError());
            ok(bytes == sizeof(evtmask), "expected %u, written %lu\n", (UINT)sizeof(evtmask), bytes);
            ok(res, "WaitCommEvent error %ld\n", GetLastError());
            ok(evtmask & EV_TXEMPTY, "WaitCommEvent: expected EV_TXEMPTY, got %#lx\n", evtmask);
        }
        else
        {
            ok(!evtmask, "WaitCommEvent: expected 0, got %#lx\n", evtmask);

            ovl_wait2.Offset = 0;
            ovl_wait2.OffsetHigh = 0;
            ovl_wait2.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            SetLastError(0xdeadbeef);
            res = WaitCommEvent(hcom, &evtmask, &ovl_wait2);
            ok(!res, "WaitCommEvent should fail if there is a pending wait\n");
            ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %ld\n", GetLastError());
            CloseHandle(ovl_wait2.hEvent);

            /* unblock pending wait */
            trace("recovering after WAIT_TIMEOUT...\n");
            res = SetCommMask(hcom, EV_TXEMPTY);
            ok(res, "SetCommMask error %ld\n", GetLastError());

            res = WaitForSingleObject(ovl_wait.hEvent, TIMEOUT);
            ok(res == WAIT_OBJECT_0, "WaitCommEvent failed with a timeout\n");
            CloseHandle(ovl_wait.hEvent);
        }

        CloseHandle(hcom);
    }
}

/* A new open handle should not return error or have bytes in the Queues */
static void test_ClearCommError(void)
{
    HANDLE hcom;
    DWORD  errors;
    COMSTAT lpStat;

    hcom = test_OpenComm(FALSE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(ClearCommError(hcom, &errors, &lpStat), "ClearCommError failed\n");
    ok(lpStat.cbInQue == 0, "Unexpected %ld chars in InQueue\n", lpStat.cbInQue);
    ok(lpStat.cbOutQue == 0, "Unexpected %ld chars in OutQueue\n", lpStat.cbOutQue);
    ok(errors == 0, "ClearCommErrors: Unexpected error 0x%08lx\n", errors);

    CloseHandle(hcom);
}

static void test_non_pending_errors(void)
{
    HANDLE hcom;
    DCB dcb;
    DWORD err;

    hcom = test_OpenComm(FALSE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    dcb.ByteSize = 255; /* likely bogus */
    ok(!SetCommState(hcom, &dcb), "SetCommState should have failed\n");
    ok(ClearCommError(hcom, &err, NULL), "ClearCommError should succeed\n");
    ok(!(err & CE_MODE), "ClearCommError shouldn't set CE_MODE byte in this case (%lx)\n", err);

    CloseHandle(hcom);
}

static void test_LoopbackRead(void)
{
    HANDLE hcom;
    DCB dcb;
    COMMTIMEOUTS timeouts;
    char rbuf[32];
    DWORD before, after, diff, read, read1, written, evtmask=0, i;
    BOOL res;
    char tbuf[]="test_LoopbackRead";

    if (!loopback_txd_rxd) return;

    hcom = test_OpenComm(FALSE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    trace("Starting test_LoopbackRead\n");
    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    dcb.BaudRate = FASTBAUD;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.fRtsControl=RTS_CONTROL_ENABLE;
    dcb.fDtrControl=DTR_CONTROL_ENABLE;
    dcb.StopBits = ONESTOPBIT;
    ok(SetCommState(hcom, &dcb), "SetCommState failed\n");

    ZeroMemory( &timeouts, sizeof(timeouts));
    timeouts.ReadTotalTimeoutConstant = TIMEOUT;
    ok(SetCommTimeouts(hcom, &timeouts),"SetCommTimeouts failed\n");

    ok(SetCommMask(hcom, EV_TXEMPTY), "SetCommMask failed\n");

    before = GetTickCount();
    ok(WriteFile(hcom,tbuf,sizeof(tbuf),&written, NULL), "WriteFile failed\n");
    after = GetTickCount();
    ok(written == sizeof(tbuf),"WriteFile %ld bytes written\n", written);
    diff = after -before;

    /* make sure all bytes are written, so Readfile will succeed in one call*/
    ok(WaitCommEvent(hcom, &evtmask, NULL), "WaitCommEvent failed\n");
    before = GetTickCount();
    ok(evtmask == EV_TXEMPTY,
                 "WaitCommEvent: Unexpected EvtMask 0x%08lx, expected 0x%08x\n",
		 evtmask, EV_TXEMPTY);
    trace("Write %ld ms WaitCommEvent EV_TXEMPTY %ld ms\n", diff, before- after);

    read=0;
    ok(ReadFile(hcom, rbuf, sizeof(rbuf), &read, NULL), "Readfile failed\n");
    ok(read == sizeof(tbuf),"ReadFile read %ld bytes, expected \"%s\"\n", read,rbuf);

    /* Now do the same with a slower Baud rate.
       As we request more characters than written, we will hit the timeout
    */

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    dcb.BaudRate = 9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.fRtsControl=RTS_CONTROL_ENABLE;
    dcb.fDtrControl=DTR_CONTROL_ENABLE;
    dcb.StopBits = ONESTOPBIT;
    ok(SetCommState(hcom, &dcb), "SetCommState failed\n");

    ok(SetCommMask(hcom, EV_RXCHAR), "SetCommMask failed\n");
    ok(WriteFile(hcom,tbuf,sizeof(tbuf),&written, NULL), "WriteFile failed\n");
    ok(written == sizeof(tbuf),"WriteFile %ld bytes written\n", written);

    trace("WaitCommEventEV_RXCHAR\n");
    ok(WaitCommEvent(hcom, &evtmask, NULL), "WaitCommEvent failed\n");
    ok(evtmask == EV_RXCHAR, "WaitCommEvent: Unexpected EvtMask 0x%08lx, expected 0x%08x\n",
       evtmask, EV_RXCHAR);

    before = GetTickCount();
    res = ReadFile(hcom, rbuf, sizeof(rbuf), &read, NULL);
    after = GetTickCount();
    ok(res, "Readfile failed\n");
    ok(read == sizeof(tbuf),"ReadFile read %ld bytes\n", read);
    diff = after - before;
    trace("Readfile for %ld chars took %ld ms\n", read, diff);
    ok( (diff > TIMEOUT - TIMEDELTA) && (diff < TIMEOUT + TIMEDELTA),
	"Timedout Wait took %ld ms, expected around %d\n", diff, TIMEOUT);

    /* now do a plain read with slow speed
     * This will result in several low level reads and a timeout to happen
     */
    dcb.BaudRate = SLOWBAUD;
    ok(SetCommState(hcom, &dcb), "SetCommState failed\n");
    ok(WriteFile(hcom,tbuf,sizeof(tbuf),&written, NULL), "WriteFile failed\n");
    before = GetTickCount();
    read = 0;
    read1 =0;
    i=0;
    do 
    {
	res = ReadFile(hcom, rbuf+read, sizeof(rbuf)-read, &read1, NULL);
	ok(res, "Readfile failed\n");
	read += read1;
	i++;
    }
    while ((read < sizeof(tbuf)) && (i <10));
    after =  GetTickCount();
    ok( read == sizeof(tbuf),"ReadFile read %ld bytes\n", read);
    trace("Plain Read for %ld char at %d baud took %ld ms\n", read, SLOWBAUD, after-before);

    CloseHandle(hcom);
}

static void test_LoopbackCtsRts(void)
{
    HANDLE hcom;
    DWORD ModemStat = 0, defaultStat = 0;
    DCB dcb;

    if (!loopback_rts_cts) return;

    hcom = test_OpenComm(FALSE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    memset (&dcb, 0, sizeof (dcb));
    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    if (dcb.fRtsControl == RTS_CONTROL_HANDSHAKE)
    {
	trace("RTS_CONTROL_HANDSHAKE is set, so don't manipulate RTS\n");
	CloseHandle(hcom);
	return;
    }
    ok(GetCommModemStatus(hcom, &defaultStat), "GetCommModemStatus failed\n");
    /* XP returns some values in the low nibble, so mask them out*/
    defaultStat &= MS_CTS_ON|MS_DSR_ON|MS_RING_ON|MS_RLSD_ON;
    if(defaultStat & MS_CTS_ON)
    {
	ok(EscapeCommFunction(hcom, CLRRTS), "EscapeCommFunction failed to clear RTS\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok ((ModemStat & MS_CTS_ON) == 0, "CTS didn't react: 0x%04lx,  expected 0x%04lx\n",
	    ModemStat, (defaultStat & ~MS_CTS_ON));
	ok(EscapeCommFunction(hcom, SETRTS), "EscapeCommFunction failed to clear RTS\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok (ModemStat ==  defaultStat, "Failed to restore CTS: 0x%04lx, expected 0x%04lx\n",
	    ModemStat, defaultStat);
    }
    else
    {
	ok(EscapeCommFunction(hcom, SETRTS), "EscapeCommFunction failed to set RTS\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok ((ModemStat & MS_CTS_ON) == MS_CTS_ON,
            "CTS didn't react: 0x%04lx,  expected 0x%04lx\n",
	    ModemStat, (defaultStat | MS_CTS_ON));
	ok(EscapeCommFunction(hcom, CLRRTS), "EscapeCommFunction failed to clear RTS\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok (ModemStat ==  defaultStat, "Failed to restore CTS: 0x%04lx, expected 0x%04lx\n",
	    ModemStat, defaultStat);
    }

    CloseHandle(hcom);
}

static void test_LoopbackDtrDcd(void)
{
    HANDLE hcom;
    DWORD ModemStat = 0, defaultStat = 0;
    DCB dcb;

    if (!loopback_dtr_dcd) return;

    hcom = test_OpenComm(FALSE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    if (dcb.fDtrControl == DTR_CONTROL_HANDSHAKE)
    {
	trace("DTR_CONTROL_HANDSHAKE is set, so don't manipulate DTR\n");
        CloseHandle(hcom);
	return;
    }
    ok(GetCommModemStatus(hcom, &defaultStat), "GetCommModemStatus failed\n");
    /* XP returns some values in the low nibble, so mask them out*/
    defaultStat &= MS_CTS_ON|MS_DSR_ON|MS_RING_ON|MS_RLSD_ON;
    if(defaultStat & MS_RLSD_ON)
    {
	ok(EscapeCommFunction(hcom, CLRDTR), "EscapeCommFunction failed to clear DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok ((ModemStat & MS_RLSD_ON) == 0, "RLSD didn't react: 0x%04lx,  expected 0x%04lx\n",
	    ModemStat, (defaultStat & ~MS_RLSD_ON));
	ok(EscapeCommFunction(hcom, SETDTR), "EscapeCommFunction failed to set DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok (ModemStat ==  defaultStat, "Failed to restore RLSD: 0x%04lx, expected 0x%04lx\n",
	    ModemStat, defaultStat);
    }
    else
    {
	ok(EscapeCommFunction(hcom, SETDTR), "EscapeCommFunction failed to set DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok ((ModemStat & MS_RLSD_ON) == MS_RLSD_ON,
            "RLSD didn't react: 0x%04lx,  expected 0x%04lx\n",
	    ModemStat, (defaultStat | MS_RLSD_ON));
	ok(EscapeCommFunction(hcom, CLRDTR), "EscapeCommFunction failed to clear DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok (ModemStat ==  defaultStat, "Failed to restore RLSD: 0x%04lx, expected 0x%04lx\n",
	    ModemStat, defaultStat);
    }

    CloseHandle(hcom);
}

static void test_LoopbackDtrDsr(void)
{
    HANDLE hcom;
    DWORD ModemStat = 0, defaultStat = 0;
    DCB dcb;

    if (!loopback_dtr_dsr) return;

    hcom = test_OpenComm(FALSE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    if (dcb.fDtrControl == DTR_CONTROL_DISABLE)
    {
	trace("DTR_CONTROL_HANDSHAKE is set, so don't manipulate DTR\n");
        CloseHandle(hcom);
	return;
    }
    ok(GetCommModemStatus(hcom, &defaultStat), "GetCommModemStatus failed\n");
    /* XP returns some values in the low nibble, so mask them out*/
    defaultStat &= MS_CTS_ON|MS_DSR_ON|MS_RING_ON|MS_RLSD_ON;
    if(defaultStat & MS_DSR_ON)
    {
	ok(EscapeCommFunction(hcom, CLRDTR), "EscapeCommFunction failed to clear DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok ((ModemStat & MS_DSR_ON) == 0, "CTS didn't react: 0x%04lx,  expected 0x%04lx\n",
	    ModemStat, (defaultStat & ~MS_DSR_ON));
	ok(EscapeCommFunction(hcom, SETDTR), "EscapeCommFunction failed to clear DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok (ModemStat ==  defaultStat, "Failed to restore DSR: 0x%04lx, expected 0x%04lx\n",
	    ModemStat, defaultStat);
    }
    else
    {
	ok(EscapeCommFunction(hcom, SETDTR), "EscapeCommFunction failed to set DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok ((ModemStat & MS_DSR_ON) == MS_DSR_ON,
            "CTS didn't react: 0x%04lx,expected 0x%04lx\n",
	    ModemStat, (defaultStat | MS_DSR_ON));
	ok(EscapeCommFunction(hcom, CLRDTR), "EscapeCommFunction failed to clear DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok (ModemStat ==  defaultStat, "Failed to restore DSR: 0x%04lx, expected 0x%04lx\n",
	    ModemStat, defaultStat);
    }

    CloseHandle(hcom);
}

static void test_LoopbackDtrRing(void)
{
    HANDLE hcom;
    DWORD ModemStat = 0, defaultStat = 0;
    DCB dcb;

    if (!loopback_dtr_ring) return;

    hcom = test_OpenComm(FALSE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    if (dcb.fDtrControl == DTR_CONTROL_HANDSHAKE)
    {
	trace("DTR_CONTROL_HANDSHAKE is set, so don't manipulate DTR\n");
	CloseHandle(hcom);
	return;
    }
    ok(GetCommModemStatus(hcom, &defaultStat), "GetCommModemStatus failed\n");
    /* XP returns some values in the low nibble, so mask them out*/
    defaultStat &= MS_CTS_ON|MS_DSR_ON|MS_RING_ON|MS_RLSD_ON;
    if(defaultStat & MS_RING_ON)
    {
	ok(EscapeCommFunction(hcom, CLRDTR), "EscapeCommFunction failed to clear DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok ((ModemStat & MS_RING_ON) == 0, "RING didn't react: 0x%04lx,  expected 0x%04lx\n",
	    ModemStat, (defaultStat & ~MS_RING_ON));
	ok(EscapeCommFunction(hcom, SETDTR), "EscapeCommFunction failed to set DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok (ModemStat ==  defaultStat, "Failed to restore RING: 0x%04lx, expected 0x%04lx\n",
	    ModemStat, defaultStat);
    }
    else
    {
	ok(EscapeCommFunction(hcom, SETDTR), "EscapeCommFunction failed to set DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok ((ModemStat & MS_RING_ON) == MS_RING_ON,
            "RING didn't react: 0x%04lx,expected 0x%04lx\n",
	    ModemStat, (defaultStat | MS_RING_ON));
	ok(EscapeCommFunction(hcom, CLRDTR), "EscapeCommFunction failed to clear DTR\n");
	ok(GetCommModemStatus(hcom, &ModemStat), "GetCommModemStatus failed\n");
	ok (ModemStat ==  defaultStat, "Failed to restore RING: 0x%04lx, expected 0x%04lx\n",
	    ModemStat, defaultStat);
    }

    CloseHandle(hcom);
}

/*
 * Set up a WaitCommEvent for anything in the receive buffer,
 * then write to TX to put a character
 * into the RX buffer
 * Need Loopback TX->RX
*/

static void test_WaitRx(void)
{
    OVERLAPPED overlapped, overlapped_w;
    HANDLE hcom, hComPortEvent, hComWriteEvent;
    DWORD before, after, after1, diff, success_wait = FALSE, success_write;
    DWORD err_wait, err_write, written, evtmask=0;

    if (!loopback_txd_rxd) return;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(SetCommMask(hcom, EV_RXCHAR), "SetCommMask failed\n");
    hComPortEvent =  CreateEventW( NULL, TRUE, FALSE, NULL );
    ok(hComPortEvent != 0, "CreateEvent failed\n");
    ZeroMemory( &overlapped, sizeof(overlapped));
    overlapped.hEvent = hComPortEvent;

    hComWriteEvent = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok(hComWriteEvent != NULL, "CreateEvent res %ld\n", GetLastError());
    ZeroMemory( &overlapped_w, sizeof(overlapped_w));
    overlapped_w.hEvent = hComWriteEvent;

    before = GetTickCount();
    success_wait = WaitCommEvent(hcom, &evtmask, &overlapped);
    err_wait = GetLastError();
    after = GetTickCount();
    trace("Success 0x%08lx err %ld evtmask 0x%08lx\n", success_wait, err_wait, evtmask);
    ok(success_wait || err_wait == ERROR_IO_PENDING, "overlapped WaitCommEvent failed\n");
    trace("overlapped WaitCommEvent returned.\n");

    success_write= WriteFile(hcom, "X", 1, &written, &overlapped_w);
    err_write = GetLastError();
    ok(success_write || err_write == ERROR_IO_PENDING,
       "overlapped WriteFile failed, err %ld\n",
       err_write);

    if (!success_write && (err_write == ERROR_IO_PENDING)) {
      success_write = WaitForSingleObjectEx(hComWriteEvent, TIMEOUT, TRUE);
      err_write = GetLastError();
      ok(success_write == WAIT_OBJECT_0, "WaitForSingleObjectEx, res %ld, err %ld\n",
	 success_write, err_write);
    }
    Sleep(TIMEOUT >>1);
    success_write = GetOverlappedResult(hcom, &overlapped_w, &written, FALSE);
    err_write = GetLastError();

    trace("Write after Wait res 0x%08lx err %ld\n",success_write, err_write);
    ok(success_write && written ==1, "Write after Wait res 0x%08lx err %ld\n",
       success_write, err_write);

    if (!success_wait && (err_wait == ERROR_IO_PENDING)) {
      success_wait = WaitForSingleObjectEx(hComPortEvent, TIMEOUT, TRUE);
      err_wait = GetLastError();
      ok(success_wait == WAIT_OBJECT_0, "wait hComPortEvent, res 0x%08lx, err %ld\n",
	 success_wait, err_wait);
    }
    success_wait = GetOverlappedResult(hcom, &overlapped, &written, FALSE);
    err_wait = GetLastError();
    after1 = GetTickCount();
    trace("Success 0x%08lx err %ld evtmask 0x%08lx diff1 %ld, diff2 %ld\n",
	  success_wait, err_wait, evtmask, after-before, after1-before);

    ok(evtmask & EV_RXCHAR, "Detect  EV_RXCHAR: 0x%08lx, expected 0x%08x\n",
       evtmask, EV_RXCHAR);
    diff = after1 - before;
    ok ((diff > (TIMEOUT>>1) -TIMEDELTA) && (diff < (TIMEOUT>>1) + TIMEDELTA),
	"Unexpected time %ld, expected around %d\n", diff, TIMEOUT>>1);

    CloseHandle(hcom);
}

/*
 * EV_RXCHAR is an event, not a state: every received character is recorded
 * in the event history, a WaitCommEvent returns and clears that history, and
 * characters already sitting in the input queue don't satisfy the next wait.
 * Need Loopback TX->RX
 */

static HANDLE rx_open(DWORD mask)
{
    COMMTIMEOUTS timeouts = { MAXDWORD, 0, 0, 0, 0 };
    HANDLE hcom;
    DCB dcb;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return hcom;

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    ok(GetCommState(hcom, &dcb), "GetCommState failed, error %lu\n", GetLastError());
    dcb.BaudRate = FASTBAUD;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    ok(SetCommState(hcom, &dcb), "SetCommState failed, error %lu\n", GetLastError());
    ok(SetCommTimeouts(hcom, &timeouts), "SetCommTimeouts failed, error %lu\n", GetLastError());
    ok(PurgeComm(hcom, PURGE_RXCLEAR | PURGE_TXCLEAR), "PurgeComm failed, error %lu\n", GetLastError());
    ok(SetCommMask(hcom, mask), "SetCommMask failed, error %lu\n", GetLastError());
    return hcom;
}

static void rx_write(HANDLE hcom, DWORD len)
{
    static const char data[] = "abcdefghijklmnop";
    OVERLAPPED ovl;
    DWORD written = 0;
    BOOL ret;

    memset(&ovl, 0, sizeof(ovl));
    ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ret = WriteFile(hcom, data, len, &written, &ovl);
    if (!ret && GetLastError() == ERROR_IO_PENDING &&
        WaitForSingleObject(ovl.hEvent, TIMEOUT) == WAIT_OBJECT_0)
        ret = GetOverlappedResult(hcom, &ovl, &written, FALSE);
    ok(ret && written == len, "WriteFile ret %d, written %lu, error %lu\n", ret, written, GetLastError());
    CloseHandle(ovl.hEvent);
}

/* wait until the loopback delivered the expected number of characters */
static DWORD rx_queue(HANDLE hcom, DWORD expected)
{
    DWORD start = GetTickCount(), errors;
    COMSTAT stat;

    for (;;)
    {
        stat.cbInQue = 0;
        if (!ClearCommError(hcom, &errors, &stat)) break;
        if (stat.cbInQue >= expected || GetTickCount() - start > TIMEOUT) break;
        Sleep(10);
    }
    return stat.cbInQue;
}

static DWORD rx_read(HANDLE hcom)
{
    OVERLAPPED ovl;
    char buf[64];
    DWORD read = 0;
    BOOL ret;

    memset(&ovl, 0, sizeof(ovl));
    ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ret = ReadFile(hcom, buf, sizeof(buf), &read, &ovl);
    if (!ret && GetLastError() == ERROR_IO_PENDING &&
        WaitForSingleObject(ovl.hEvent, TIMEOUT) == WAIT_OBJECT_0)
        ret = GetOverlappedResult(hcom, &ovl, &read, FALSE);
    ok(ret, "ReadFile failed, error %lu\n", GetLastError());
    CloseHandle(ovl.hEvent);
    return read;
}

static void rx_wait_start(HANDLE hcom, OVERLAPPED *ovl, DWORD *evtmask)
{
    BOOL ret;

    *evtmask = 0;
    ResetEvent(ovl->hEvent);
    ret = WaitCommEvent(hcom, evtmask, ovl);
    ok(ret || GetLastError() == ERROR_IO_PENDING, "WaitCommEvent failed, error %lu\n", GetLastError());
}

/* returns FALSE if the wait is still pending after the timeout */
static BOOL rx_wait_done(HANDLE hcom, OVERLAPPED *ovl, DWORD timeout)
{
    DWORD bytes;

    if (!HasOverlappedIoCompleted(ovl)) WaitForSingleObject(ovl->hEvent, timeout);
    if (!HasOverlappedIoCompleted(ovl)) return FALSE;
    ok(GetOverlappedResult(hcom, ovl, &bytes, FALSE), "WaitCommEvent failed, error %lu\n", GetLastError());
    return TRUE;
}

/* complete a wait still pending with a new character, then drop everything */
static void rx_close(HANDLE hcom, OVERLAPPED *ovl)
{
    if (!HasOverlappedIoCompleted(ovl))
    {
        rx_write(hcom, 1);
        if (!rx_wait_done(hcom, ovl, TIMEOUT))
        {
            ok(0, "WaitCommEvent not completed by a new character\n");
            CancelIo(hcom);
            WaitForSingleObject(ovl->hEvent, TIMEOUT);
        }
    }
    PurgeComm(hcom, PURGE_RXCLEAR | PURGE_TXCLEAR);
    CloseHandle(hcom);
}

static void test_WaitRxHistory(void)
{
    OVERLAPPED ovl;
    HANDLE hcom;
    DWORD evtmask, queued;
    BOOL done;

    if (!loopback_txd_rxd) return;

    memset(&ovl, 0, sizeof(ovl));
    ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* a pending wait completes on arrival; the queued character doesn't satisfy the next one */
    winetest_push_context("pending");
    if ((hcom = rx_open(EV_RXCHAR)) != INVALID_HANDLE_VALUE)
    {
        rx_wait_start(hcom, &ovl, &evtmask);
        ok(!rx_wait_done(hcom, &ovl, 200), "wait with an empty queue completed, evtmask 0x%08lx\n", evtmask);
        rx_write(hcom, 1);
        done = rx_wait_done(hcom, &ovl, TIMEOUT);
        ok(done && (evtmask & EV_RXCHAR), "arrival didn't complete the wait: done %d, evtmask 0x%08lx\n", done, evtmask);
        queued = rx_queue(hcom, 1);
        ok(queued == 1, "got %lu characters, expected 1\n", queued);

        rx_wait_start(hcom, &ovl, &evtmask);
        done = rx_wait_done(hcom, &ovl, 200);
        ok(!done, "wait completed for a character already reported, evtmask 0x%08lx\n", evtmask);
        if (!done)
        {
            rx_write(hcom, 1);
            done = rx_wait_done(hcom, &ovl, TIMEOUT);
            ok(done && (evtmask & EV_RXCHAR), "new character didn't complete the wait: done %d, evtmask 0x%08lx\n",
               done, evtmask);
            queued = rx_queue(hcom, 2);
            ok(queued == 2, "got %lu characters, expected 2\n", queued);
        }
        rx_close(hcom, &ovl);
    }
    winetest_pop_context();

    /* characters received without a pending wait are one event for the next wait */
    winetest_push_context("history");
    if ((hcom = rx_open(EV_RXCHAR)) != INVALID_HANDLE_VALUE)
    {
        rx_write(hcom, 8);
        queued = rx_queue(hcom, 8);
        ok(queued == 8, "got %lu characters, expected 8\n", queued);
        rx_wait_start(hcom, &ovl, &evtmask);
        done = rx_wait_done(hcom, &ovl, 100);
        ok(done && (evtmask & EV_RXCHAR), "history not reported: done %d, evtmask 0x%08lx\n", done, evtmask);

        rx_wait_start(hcom, &ovl, &evtmask);
        done = rx_wait_done(hcom, &ovl, 200);
        ok(!done, "history reported twice, evtmask 0x%08lx\n", evtmask);
        if (!done)
        {
            rx_write(hcom, 1);
            done = rx_wait_done(hcom, &ovl, TIMEOUT);
            ok(done && (evtmask & EV_RXCHAR), "new character didn't complete the wait: done %d, evtmask 0x%08lx\n",
               done, evtmask);
            queued = rx_queue(hcom, 9);
            ok(queued == 9, "got %lu characters, expected 9\n", queued);
            ok(rx_read(hcom) == 9, "lost characters\n");
        }
        rx_close(hcom, &ovl);
    }
    winetest_pop_context();

    /* reading the characters doesn't clear the history */
    winetest_push_context("read");
    if ((hcom = rx_open(EV_RXCHAR)) != INVALID_HANDLE_VALUE)
    {
        rx_write(hcom, 4);
        queued = rx_queue(hcom, 4);
        ok(queued == 4, "got %lu characters, expected 4\n", queued);
        ok(rx_read(hcom) == 4, "lost characters\n");
        rx_wait_start(hcom, &ovl, &evtmask);
        done = rx_wait_done(hcom, &ovl, 100);
        ok(done && (evtmask & EV_RXCHAR), "history lost after ReadFile: done %d, evtmask 0x%08lx\n", done, evtmask);
        rx_close(hcom, &ovl);
    }
    winetest_pop_context();

    /* SetCommMask clears the history, even for the events still in the mask (usbser.sys) */
    winetest_push_context("mask reset");
    if ((hcom = rx_open(EV_RXCHAR)) != INVALID_HANDLE_VALUE)
    {
        rx_write(hcom, 1);
        queued = rx_queue(hcom, 1);
        ok(queued == 1, "got %lu characters, expected 1\n", queued);
        ok(SetCommMask(hcom, EV_RXCHAR | EV_CTS), "SetCommMask failed, error %lu\n", GetLastError());
        rx_wait_start(hcom, &ovl, &evtmask);
        done = rx_wait_done(hcom, &ovl, 200);
        ok(!done, "history kept across SetCommMask, evtmask 0x%08lx\n", evtmask);
        rx_close(hcom, &ovl);
    }
    winetest_pop_context();

    winetest_push_context("mask dropped");
    if ((hcom = rx_open(EV_RXCHAR)) != INVALID_HANDLE_VALUE)
    {
        rx_write(hcom, 1);
        queued = rx_queue(hcom, 1);
        ok(queued == 1, "got %lu characters, expected 1\n", queued);
        ok(SetCommMask(hcom, EV_CTS), "SetCommMask failed, error %lu\n", GetLastError());
        ok(SetCommMask(hcom, EV_RXCHAR), "SetCommMask failed, error %lu\n", GetLastError());
        rx_wait_start(hcom, &ovl, &evtmask);
        done = rx_wait_done(hcom, &ovl, 200);
        ok(!done, "history kept by a mask without EV_RXCHAR, evtmask 0x%08lx\n", evtmask);
        rx_close(hcom, &ovl);
    }
    winetest_pop_context();

    winetest_push_context("mask off");
    if ((hcom = rx_open(EV_CTS)) != INVALID_HANDLE_VALUE)
    {
        rx_write(hcom, 1);
        queued = rx_queue(hcom, 1);
        ok(queued == 1, "got %lu characters, expected 1\n", queued);
        ok(SetCommMask(hcom, EV_RXCHAR), "SetCommMask failed, error %lu\n", GetLastError());
        rx_wait_start(hcom, &ovl, &evtmask);
        done = rx_wait_done(hcom, &ovl, 200);
        ok(!done, "character received without EV_RXCHAR in the mask reported, evtmask 0x%08lx\n", evtmask);
        rx_close(hcom, &ovl);
    }
    winetest_pop_context();

    /* PURGE_RXCLEAR drops the characters, not the history */
    winetest_push_context("purge");
    if ((hcom = rx_open(EV_RXCHAR)) != INVALID_HANDLE_VALUE)
    {
        rx_write(hcom, 1);
        queued = rx_queue(hcom, 1);
        ok(queued == 1, "got %lu characters, expected 1\n", queued);
        ok(PurgeComm(hcom, PURGE_RXCLEAR), "PurgeComm failed, error %lu\n", GetLastError());
        queued = rx_queue(hcom, 0);
        ok(queued == 0, "got %lu characters after PURGE_RXCLEAR\n", queued);
        rx_wait_start(hcom, &ovl, &evtmask);
        done = rx_wait_done(hcom, &ovl, 100);
        ok(done && (evtmask & EV_RXCHAR), "history lost after PURGE_RXCLEAR: done %d, evtmask 0x%08lx\n",
           done, evtmask);
        rx_close(hcom, &ovl);
    }
    winetest_pop_context();

    CloseHandle(ovl.hEvent);
}

/* Change the controlling line after the given timeout to the given state
   By the loopback, this should trigger the WaitCommEvent
*/
static DWORD CALLBACK toggle_ctlLine(LPVOID arg)
{
    DWORD_PTR *args = arg;
    DWORD timeout = args[0];
    DWORD ctl     = args[1];
    HANDLE hcom   = (HANDLE) args[2];
    HANDLE hComPortEvent = (HANDLE) args[3];
    DWORD success, err;

    trace("toggle_ctlLine timeout %ld ctl 0x%08lx handle %p\n", timeout, ctl, hcom );
    Sleep(timeout);
    ok(EscapeCommFunction(hcom, ctl),"EscapeCommFunction 0x%08lx failed\n", ctl);
    trace("toggle_ctline done\n");
    success = WaitForSingleObjectEx(hComPortEvent, TIMEOUT, TRUE);
    err = GetLastError();
    trace("toggle_ctline WaitForSingleObjectEx res 0x%08lx err %ld\n",
	  success, err);
    return 0;
}

/*
 * Wait for a change in CTS
 * Needs Loopback from DTR to CTS
 */
static void test_WaitCts(void)
{
    DCB dcb;
    OVERLAPPED overlapped;
    HANDLE hcom, hComPortEvent, alarmThread;
    DWORD_PTR args[4];
    DWORD alarmThreadId, before, after, after1, diff, success, err, written, evtmask=0, defaultStat = 0;

    if (!loopback_rts_cts) return;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    dcb.fRtsControl=RTS_CONTROL_ENABLE;
    dcb.fDtrControl=DTR_CONTROL_ENABLE;
    ok(SetCommState(hcom, &dcb), "SetCommState failed\n");
    if (dcb.fDtrControl == RTS_CONTROL_DISABLE)
    {
	trace("RTS_CONTROL_HANDSHAKE is set, so don't manipulate DTR\n");
        CloseHandle(hcom);
	return;
    }
    args[0]= TIMEOUT >>1;
    ok(GetCommModemStatus(hcom, &defaultStat), "GetCommModemStatus failed\n");
    if(defaultStat & MS_CTS_ON)
	args[1] = CLRRTS;
    else
	args[1] = SETRTS;
    args[2]=(DWORD_PTR)hcom;

    trace("test_WaitCts timeout %Id clt 0x%08Ix handle %p\n",args[0], args[1], hcom);

    ok(SetCommMask(hcom, EV_CTS), "SetCommMask failed\n");
    hComPortEvent =  CreateEventW( NULL, TRUE, FALSE, NULL );
    ok(hComPortEvent != 0, "CreateEvent failed\n");
    args[3] = (DWORD_PTR)hComPortEvent;
    alarmThread = CreateThread(NULL, 0, toggle_ctlLine, args, 0, &alarmThreadId);
    /* Wait a minimum to let the thread start up */
    Sleep(10);
    trace("Thread created\n");
    ok(alarmThread !=0 , "CreateThread Failed\n");

    ZeroMemory( &overlapped, sizeof(overlapped));
    overlapped.hEvent = hComPortEvent;
    before = GetTickCount();
    success = WaitCommEvent(hcom, &evtmask, &overlapped);
    err = GetLastError();
    after = GetTickCount();

    trace("Success 0x%08lx err %ld evtmask 0x%08lx\n", success, err, evtmask);
    ok(success || err == ERROR_IO_PENDING, "overlapped WaitCommEvent failed\n");
    trace("overlapped WaitCommEvent returned.\n");
    if (!success && (err == ERROR_IO_PENDING))
	ok(WaitForSingleObjectEx(hComPortEvent, TIMEOUT, TRUE) == 0,
		     "WaitCts hComPortEvent failed\n");
    success = GetOverlappedResult(hcom, &overlapped, &written, FALSE);
    err = GetLastError();
    after1 = GetTickCount();
    trace("Success 0x%08lx err %ld evtmask 0x%08lx diff1 %ld, diff2 %ld\n",
	  success, err, evtmask, after-before, after1-before);

    ok(evtmask & EV_CTS, "Failed to detect  EV_CTS: 0x%08lx, expected 0x%08x\n",
		 evtmask, EV_CTS);
    ok(GetCommModemStatus(hcom, &evtmask), "GetCommModemStatus failed\n");
    if(defaultStat & MS_CTS_ON)
	ok((evtmask & MS_CTS_ON) == 0,"CTS didn't change state!\n");
    else
	ok((evtmask & MS_CTS_ON), "CTS didn't change state!\n");

    diff = after1 - before;
    ok ((diff > (TIMEOUT>>1) -TIMEDELTA) && (diff < (TIMEOUT>>1) + TIMEDELTA),
                  "Unexpected time %ld, expected around %d\n", diff, TIMEOUT>>1);

    /*restore RTS Settings*/
    if(defaultStat & MS_CTS_ON)
	args[1] = SETRTS;
    else
	args[1] = CLRRTS;

    CloseHandle(hcom);
    ok( !WaitForSingleObject( alarmThread, 10000 ), "thread still running\n" );
    CloseHandle( alarmThread );
}

/* Change the  Comm Mask while a Wait is going on
   WaitCommevent should return with a EVTMASK set to zero
*/
static DWORD CALLBACK reset_CommMask(LPVOID arg)
{
    DWORD_PTR *args = arg;
    DWORD timeout = args[0];
    HANDLE hcom   = (HANDLE) args[1];

    trace(" Changing CommMask on the fly for handle %p after timeout %ld\n",
	  hcom, timeout);
    Sleep(timeout);
    ok(SetCommMask(hcom, 0),"SetCommMask %p failed\n", hcom);
    trace("SetCommMask changed\n");
    return 0;
}

/* Set up a Wait for a change on CTS. We don't toggle any line, but we
   reset the CommMask and expect the wait to return with a mask of 0
   No special port connections needed
*/
static void test_AbortWaitCts(void)
{
    DCB dcb;
    OVERLAPPED overlapped;
    HANDLE hcom, hComPortEvent, alarmThread;
    DWORD_PTR args[2];
    DWORD alarmThreadId, before, after, after1, diff, success, err, written, evtmask=0;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    if (dcb.fDtrControl == RTS_CONTROL_DISABLE)
    {
	trace("RTS_CONTROL_HANDSHAKE is set, so don't manipulate DTR\n");
        CloseHandle(hcom);
	return;
    }
    args[0]= TIMEOUT >>1;
    args[1]= (DWORD_PTR)hcom;

    trace("test_AbortWaitCts timeout %Id handle %p\n",args[0], hcom);

    ok(SetCommMask(hcom, EV_CTS), "SetCommMask failed\n");
    hComPortEvent =  CreateEventW( NULL, TRUE, FALSE, NULL );
    ok(hComPortEvent != 0, "CreateEvent failed\n");
    alarmThread = CreateThread(NULL, 0, reset_CommMask, args, 0, &alarmThreadId);
    /* Wait a minimum to let the thread start up */
    Sleep(10);
    trace("Thread created\n");
    ok(alarmThread !=0 , "CreateThread Failed\n");

    ZeroMemory( &overlapped, sizeof(overlapped));
    overlapped.hEvent = hComPortEvent;
    before = GetTickCount();
    success = WaitCommEvent(hcom, &evtmask, &overlapped);
    err = GetLastError();
    after = GetTickCount();

    trace("Success 0x%08lx err %ld evtmask 0x%08lx\n", success, err, evtmask);
    ok(success || err == ERROR_IO_PENDING, "overlapped WaitCommEvent failed\n");
    trace("overlapped WaitCommEvent returned.\n");
    if (!success && (err == ERROR_IO_PENDING))
	ok(WaitForSingleObjectEx(hComPortEvent, TIMEOUT, TRUE) == 0,
		     "AbortWaitCts hComPortEvent failed\n");
    success = GetOverlappedResult(hcom, &overlapped, &written, FALSE);
    err = GetLastError();
    after1 = GetTickCount();
    trace("Success 0x%08lx err %ld evtmask 0x%08lx diff1 %ld, diff2 %ld\n",
	  success, err, evtmask, after-before, after1-before);

    ok(evtmask == 0, "Incorrect EventMask 0x%08lx returned on Wait aborted bu SetCommMask, expected 0x%08x\n",
		 evtmask, 0);
    ok(GetCommModemStatus(hcom, &evtmask), "GetCommModemStatus failed\n");
    diff = after1 - before;
    ok ((diff > (TIMEOUT>>1) -TIMEDELTA) && (diff < (TIMEOUT>>1) + TIMEDELTA),
                  "Unexpected time %ld, expected around %d\n", diff, TIMEOUT>>1);

    CloseHandle(hcom);
    ok( !WaitForSingleObject( alarmThread, 10000 ), "thread still running\n" );
    CloseHandle( alarmThread );
}

struct abort_wait_args
{
    HANDLE hcom;
    OVERLAPPED *ovl;
    HANDLE pending;
    unsigned int mode;
    DWORD delay;
    BOOL ret;
};

static DWORD CALLBACK abort_wait_thread(void *arg)
{
    struct abort_wait_args *args = arg;

    WaitForSingleObject(args->pending, INFINITE);
    Sleep(args->delay);
    if (args->mode == 0) args->ret = SetCommMask(args->hcom, 0);
    else if (args->mode == 1) args->ret = SetCommMask(args->hcom, EV_CTS);
    else args->ret = CancelIoEx(args->hcom, args->ovl);
    return 0;
}

/* Start an overlapped WaitCommEvent for EV_CTS. FALSE if it did not stay
   pending (a real CTS event, nothing to abort). */
static BOOL start_pending_wait(HANDLE hcom, OVERLAPPED *ovl, DWORD *evtmask)
{
    BOOL ret;

    ret = SetCommMask(hcom, EV_CTS);
    ok(ret, "SetCommMask failed, error %lu\n", GetLastError());
    *evtmask = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    ret = WaitCommEvent(hcom, evtmask, ovl);
    if (!ret && GetLastError() == ERROR_IO_PENDING) return TRUE;
    trace("WaitCommEvent returned %d, error %lu, mask %#lx\n", ret, GetLastError(), *evtmask);
    return FALSE;
}

/* Change the CommMask, or cancel the wait, from another thread after a
   short delay (0-3 ms) while an overlapped WaitCommEvent is pending. The
   wait must always complete, and CancelIoEx must always find it.
   No special port connections needed
*/
static void test_AbortWaitLoop(void)
{
    static const char * const mode_name[] = { "SetCommMask(0)", "SetCommMask(EV_CTS)", "CancelIoEx" };
    /* static: a wait that is never completed must not point into a dead stack frame */
    static OVERLAPPED ovl;
    struct abort_wait_args args;
    unsigned int mode, i, k, count, lost, failed, status;
    DWORD evtmask, bytes;
    HANDLE hcom, thread;
    BOOL ret, waited, completed;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    args.hcom = hcom;
    args.ovl = &ovl;
    args.pending = CreateEventW(NULL, FALSE, FALSE, NULL);
    ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    for (mode = 0; mode < ARRAY_SIZE(mode_name); mode++)
    {
        count = lost = failed = status = 0;
        for (i = 0; i < 200; i++)
        {
            args.mode = mode;
            args.delay = i % 4;
            args.ret = FALSE;
            thread = CreateThread(NULL, 0, abort_wait_thread, &args, 0, NULL);
            ok(thread != NULL, "CreateThread failed, error %lu\n", GetLastError());

            waited = start_pending_wait(hcom, &ovl, &evtmask);
            if (waited)
            {
                count++;
                SetEvent(args.pending);
                completed = !WaitForSingleObject(ovl.hEvent, 500);
                if (!completed)
                {
                    lost++;
                    /* the cleanup can hit the same race, so insist */
                    for (k = 0; k < 5; k++)
                    {
                        SetCommMask(hcom, 0);
                        CancelIoEx(hcom, &ovl);
                        if (!WaitForSingleObject(ovl.hEvent, 500)) break;
                    }
                    if (k == 5)
                    {
                        ok(0, "%s: wait still pending after 5 cancellations\n", mode_name[mode]);
                        WaitForSingleObject(thread, INFINITE);
                        CloseHandle(thread);
                        CloseHandle(hcom);
                        return;
                    }
                }
                bytes = 0xdeadbeef;
                SetLastError(0xdeadbeef);
                ret = GetOverlappedResult(hcom, &ovl, &bytes, FALSE);
                if (completed && ret && evtmask == EV_CTS)
                    trace("%s: a real CTS event before the abort\n", mode_name[mode]);
                else if (mode == 2 && (ret || GetLastError() != ERROR_OPERATION_ABORTED)) status++;
                /* SetCommMask completes the wait successfully, with an empty event mask */
                else if (mode != 2 && completed && (!ret || bytes != sizeof(evtmask) || evtmask)) status++;
            }
            else
            {
                /* a real CTS event, nothing to abort */
                SetEvent(args.pending);
            }
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
            if (waited && !args.ret) failed++;
        }
        ok(count > 0, "%s: no pending wait\n", mode_name[mode]);
        ok(!lost, "%s: %u of %u pending waits did not complete\n", mode_name[mode], lost, count);
        ok(!failed, "%s failed %u of %u times\n", mode_name[mode], failed, count);
        if (mode == 2)
            ok(!status, "CancelIoEx: %u of %u waits not completed with ERROR_OPERATION_ABORTED\n", status, count);
        else
            ok(!status, "%s: %u of %u waits not completed successfully with an empty mask\n",
               mode_name[mode], status, count);
    }

    SetCommMask(hcom, 0);
    CloseHandle(ovl.hEvent);
    CloseHandle(args.pending);
    CloseHandle(hcom);
}

/* Change the CommMask from the same thread while an overlapped WaitCommEvent
   is pending. The wait completes successfully with an empty event mask, both
   for a new mask and for the same mask.
   No special port connections needed
*/
static void test_AbortWaitMask(void)
{
    static const DWORD new_mask[] = { 0, EV_CTS };
    /* static: a wait that is never completed must not point into a dead stack frame */
    static OVERLAPPED ovl;
    DWORD evtmask, bytes;
    unsigned int i;
    HANDLE hcom;
    BOOL ret;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    for (i = 0; i < ARRAY_SIZE(new_mask); i++)
    {
        winetest_push_context("SetCommMask(%#lx)", new_mask[i]);

        if (!start_pending_wait(hcom, &ovl, &evtmask))
        {
            skip("a real CTS event, nothing to abort\n");
            winetest_pop_context();
            continue;
        }

        ret = SetCommMask(hcom, new_mask[i]);
        ok(ret, "SetCommMask failed, error %lu\n", GetLastError());
        ret = !WaitForSingleObject(ovl.hEvent, TIMEOUT);
        ok(ret, "wait not completed\n");
        if (!ret)
        {
            CancelIoEx(hcom, &ovl);
            WaitForSingleObject(ovl.hEvent, TIMEOUT);
            winetest_pop_context();
            continue;
        }
        bytes = 0xdeadbeef;
        SetLastError(0xdeadbeef);
        ret = GetOverlappedResult(hcom, &ovl, &bytes, FALSE);
        if (ret && evtmask == EV_CTS)
        {
            skip("a real CTS event before the abort\n");
            winetest_pop_context();
            continue;
        }
        ok(ret, "GetOverlappedResult failed, error %lu\n", GetLastError());
        ok(bytes == sizeof(evtmask), "got %lu bytes\n", bytes);
        ok(!evtmask, "got event mask %#lx\n", evtmask);

        winetest_pop_context();
    }

    SetCommMask(hcom, 0);
    CloseHandle(ovl.hEvent);
    CloseHandle(hcom);
}

/* Close the handle while an overlapped WaitCommEvent is pending. The wait is
   cancelled and the port can be opened again.
   No special port connections needed
*/
static void test_CloseWaitPending(void)
{
    /* static: a wait that is never completed must not point into a dead stack frame */
    static OVERLAPPED ovl;
    DWORD evtmask;
    HANDLE hcom;
    BOOL ret;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!start_pending_wait(hcom, &ovl, &evtmask))
    {
        skip("a real CTS event, nothing to abort\n");
        SetCommMask(hcom, 0);
        CloseHandle(ovl.hEvent);
        CloseHandle(hcom);
        return;
    }

    CloseHandle(hcom);
    ret = !WaitForSingleObject(ovl.hEvent, TIMEOUT);
    ok(ret, "wait not completed\n");
    if (ret)
    {
        ok((NTSTATUS)ovl.Internal == STATUS_CANCELLED, "got status %#Ix\n", ovl.Internal);
        ok(!ovl.InternalHigh, "got %Iu bytes\n", ovl.InternalHigh);
    }

    hcom = test_OpenComm(TRUE);
    ok(hcom != INVALID_HANDLE_VALUE, "port not opened again, error %lu\n", GetLastError());
    if (hcom != INVALID_HANDLE_VALUE) CloseHandle(hcom);
    if (ret) CloseHandle(ovl.hEvent);
}

static const char * const abort_name[] = { "SetCommMask(0)", "CancelIoEx", "CloseHandle" };

/* Abort a pending wait with SetCommMask(0), CancelIoEx or CloseHandle (which
   closes hcom) and check how it completes. */
static void check_abort_pending_wait(HANDLE hcom, OVERLAPPED *ovl, DWORD *evtmask, unsigned int action)
{
    DWORD bytes;
    BOOL ret;

    SetLastError(0xdeadbeef);
    if (action == 0) ret = SetCommMask(hcom, 0);
    else if (action == 1) ret = CancelIoEx(hcom, ovl);
    else ret = CloseHandle(hcom);
    ok(ret, "%s failed, error %lu\n", abort_name[action], GetLastError());

    ret = !WaitForSingleObject(ovl->hEvent, TIMEOUT);
    ok(ret, "wait not completed\n");
    if (!ret)
    {
        if (action != 2)
        {
            CancelIoEx(hcom, ovl);
            WaitForSingleObject(ovl->hEvent, TIMEOUT);
        }
        return;
    }
    if (action == 2)
    {
        ok((NTSTATUS)ovl->Internal == STATUS_CANCELLED, "got status %#Ix\n", ovl->Internal);
        ok(!ovl->InternalHigh, "got %Iu bytes\n", ovl->InternalHigh);
        return;
    }

    bytes = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    ret = GetOverlappedResult(hcom, ovl, &bytes, FALSE);
    if (ret && *evtmask == EV_CTS)
        skip("a real CTS event before the abort\n");
    else if (action == 0)
    {
        ok(ret, "GetOverlappedResult failed, error %lu\n", GetLastError());
        ok(bytes == sizeof(*evtmask), "got %lu bytes\n", bytes);
        ok(!*evtmask, "got event mask %#lx\n", *evtmask);
    }
    else
        ok(!ret && GetLastError() == ERROR_OPERATION_ABORTED, "got %d, error %lu\n", ret, GetLastError());
}

/* A wait on a handle without write access stays pending, and is aborted like
   any other. The port is exclusive, so the handle is a duplicate with less
   access of the one test_OpenComm opened.
   No special port connections needed
*/
static void test_ReadOnlyWaitPending(void)
{
    /* static: a wait that is never completed must not point into a dead stack frame */
    static OVERLAPPED ovl;
    unsigned int action;
    HANDLE hcom, hro;
    DWORD evtmask, wait;
    BOOL ret;

    ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (action = 0; action < ARRAY_SIZE(abort_name); action++)
    {
        winetest_push_context("%s", abort_name[action]);

        hcom = test_OpenComm(TRUE);
        if (hcom == INVALID_HANDLE_VALUE)
        {
            ok(!action, "port not opened again, error %lu\n", GetLastError());
            winetest_pop_context();
            break;
        }
        ret = DuplicateHandle(GetCurrentProcess(), hcom, GetCurrentProcess(), &hro, FILE_GENERIC_READ, FALSE, 0);
        ok(ret, "DuplicateHandle failed, error %lu\n", GetLastError());
        CloseHandle(hcom);

        if (!start_pending_wait(hro, &ovl, &evtmask))
        {
            skip("a real CTS event, nothing to abort\n");
            SetCommMask(hro, 0);
            CloseHandle(hro);
            winetest_pop_context();
            continue;
        }

        wait = WaitForSingleObject(ovl.hEvent, 100);
        if (!wait && !ovl.Internal && evtmask == EV_CTS)
            skip("a real CTS event\n");
        else
        {
            ok(wait == WAIT_TIMEOUT, "wait completed, status %#Ix, mask %#lx\n", ovl.Internal, evtmask);
            if (wait == WAIT_TIMEOUT) check_abort_pending_wait(hro, &ovl, &evtmask, action);
        }
        if (action != 2 || wait != WAIT_TIMEOUT)
        {
            SetCommMask(hro, 0);
            CloseHandle(hro);
        }

        winetest_pop_context();
    }
    CloseHandle(ovl.hEvent);
}

/* Close the handle a wait was started on while a duplicate of it is still
   open. The wait stays pending, and is aborted through the duplicate.
   No special port connections needed
*/
static void test_CloseWaitDuplicate(void)
{
    /* static: a wait that is never completed must not point into a dead stack frame */
    static OVERLAPPED ovl;
    unsigned int action;
    HANDLE hcom, hdup;
    DWORD evtmask, wait;
    BOOL ret;

    ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (action = 0; action < ARRAY_SIZE(abort_name); action++)
    {
        winetest_push_context("%s", abort_name[action]);

        hcom = test_OpenComm(TRUE);
        if (hcom == INVALID_HANDLE_VALUE)
        {
            ok(!action, "port not opened again, error %lu\n", GetLastError());
            winetest_pop_context();
            break;
        }
        ret = DuplicateHandle(GetCurrentProcess(), hcom, GetCurrentProcess(), &hdup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        ok(ret, "DuplicateHandle failed, error %lu\n", GetLastError());

        if (!start_pending_wait(hcom, &ovl, &evtmask))
        {
            skip("a real CTS event, nothing to abort\n");
            SetCommMask(hcom, 0);
            CloseHandle(hcom);
            CloseHandle(hdup);
            winetest_pop_context();
            continue;
        }

        CloseHandle(hcom);
        wait = WaitForSingleObject(ovl.hEvent, 100);
        if (!wait && !ovl.Internal && evtmask == EV_CTS)
            skip("a real CTS event\n");
        else
        {
            todo_wine ok(wait == WAIT_TIMEOUT, "wait completed, status %#Ix, mask %#lx\n", ovl.Internal, evtmask);
            if (wait == WAIT_TIMEOUT) check_abort_pending_wait(hdup, &ovl, &evtmask, action);
        }
        if (action != 2 || wait != WAIT_TIMEOUT)
        {
            SetCommMask(hdup, 0);
            CloseHandle(hdup);
        }

        winetest_pop_context();
    }
    CloseHandle(ovl.hEvent);
}

/*
 * Wait for a change in DSR
 * Needs Loopback from DTR to DSR
 */
static void test_WaitDsr(void)
{
    DCB dcb;
    OVERLAPPED overlapped;
    HANDLE hcom, hComPortEvent, alarmThread;
    DWORD_PTR args[3];
    DWORD alarmThreadId, before, after, after1, diff, success, err, written, evtmask=0, defaultStat = 0;

    if (!loopback_dtr_dsr) return;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    if (dcb.fDtrControl == DTR_CONTROL_DISABLE)
    {
	trace("DTR_CONTROL_HANDSHAKE is set, so don't manipulate DTR\n");
        CloseHandle(hcom);
	return;
    }
    args[0]= TIMEOUT >>1;
    ok(GetCommModemStatus(hcom, &defaultStat), "GetCommModemStatus failed\n");
    if(defaultStat & MS_DSR_ON)
	args[1] = CLRDTR;
    else
	args[1] = SETDTR;
    args[2]= (DWORD_PTR)hcom;

    trace("test_WaitDsr timeout %Id clt 0x%08Ix handle %p\n",args[0], args[1], hcom);

    ok(SetCommMask(hcom, EV_DSR), "SetCommMask failed\n");
    hComPortEvent =  CreateEventW( NULL, TRUE, FALSE, NULL );
    ok(hComPortEvent != 0, "CreateEvent failed\n");
    alarmThread = CreateThread(NULL, 0, toggle_ctlLine, args, 0, &alarmThreadId);
    ok(alarmThread !=0 , "CreateThread Failed\n");

    ZeroMemory( &overlapped, sizeof(overlapped));
    overlapped.hEvent = hComPortEvent;
    before = GetTickCount();
    success = WaitCommEvent(hcom, &evtmask, &overlapped);
    err = GetLastError();
    after = GetTickCount();

    trace("Success 0x%08lx err %ld evtmask 0x%08lx\n", success, err, evtmask);
    ok(success || err == ERROR_IO_PENDING, "overlapped WaitCommEvent failed\n");
    trace("overlapped WaitCommEvent returned.\n");
    if (!success && (err == ERROR_IO_PENDING))
	ok(WaitForSingleObjectEx(hComPortEvent, TIMEOUT, TRUE) == 0,
		     "wait hComPortEvent failed\n");
    success = GetOverlappedResult(hcom, &overlapped, &written, FALSE);
    err = GetLastError();
    after1 = GetTickCount();
    trace("Success 0x%08lx err %ld evtmask 0x%08lx diff1 %ld, diff2 %ld\n",
	  success, err, evtmask, after-before, after1-before);

    ok(evtmask & EV_DSR, "Failed to detect  EV_DSR: 0x%08lx, expected 0x%08x\n",
		 evtmask, EV_DSR);
    ok(GetCommModemStatus(hcom, &evtmask), "GetCommModemStatus failed\n");
    if(defaultStat & MS_DSR_ON)
	ok((evtmask & MS_DSR_ON) == 0,"DTR didn't change state!\n");
    else
	ok((evtmask & MS_DSR_ON), "DTR didn't change state!\n");

    diff = after1 - before;
    ok ((diff > (TIMEOUT>>1) -TIMEDELTA) && (diff < (TIMEOUT>>1) + TIMEDELTA),
                  "Unexpected time %ld, expected around %d\n", diff, TIMEOUT>>1);

    /*restore RTS Settings*/
    if(defaultStat & MS_DSR_ON)
	args[1] = SETDTR;
    else
	args[1] = CLRDTR;

    CloseHandle(hcom);
    ok( !WaitForSingleObject( alarmThread, 10000 ), "thread still running\n" );
    CloseHandle( alarmThread );
}

/*
 * Wait for a Ring
 * Needs Loopback from DTR to RING
 */
static void test_WaitRing(void)
{
    DCB dcb;
    OVERLAPPED overlapped;
    HANDLE hcom, hComPortEvent, alarmThread;
    DWORD_PTR args[3];
    DWORD alarmThreadId, before, after, after1, diff, success, err, written, evtmask=0, defaultStat;
    BOOL ret;

    if (!loopback_dtr_ring) return;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    if (dcb.fDtrControl == DTR_CONTROL_DISABLE)
    {
	trace("DTR_CONTROL_HANDSHAKE is set, so don't manipulate DTR\n");
        CloseHandle(hcom);
	return;
    }
    args[0]= TIMEOUT >>1;
    ok((ret = GetCommModemStatus(hcom, &defaultStat)), "GetCommModemStatus failed\n");
    if (!ret) {
	skip("modem status failed -> skip.\n");
	CloseHandle(hcom);
	return;
    }
    if(defaultStat & MS_RING_ON)
	args[1] = CLRDTR;
    else
	args[1] = SETDTR;
    args[2]=(DWORD_PTR) hcom;

    trace("test_WaitRing timeout %Id clt 0x%08Ix handle %p\n",args[0], args[1], hcom);

    ok(SetCommMask(hcom, EV_RING), "SetCommMask failed\n");
    hComPortEvent =  CreateEventW( NULL, TRUE, FALSE, NULL );
    ok(hComPortEvent != 0, "CreateEvent failed\n");
    alarmThread = CreateThread(NULL, 0, toggle_ctlLine, args, 0, &alarmThreadId);
    ok(alarmThread !=0 , "CreateThread Failed\n");

    ZeroMemory( &overlapped, sizeof(overlapped));
    overlapped.hEvent = hComPortEvent;
    before = GetTickCount();
    success = WaitCommEvent(hcom, &evtmask, &overlapped);
    err = GetLastError();
    after = GetTickCount();

    trace("Success 0x%08lx err %ld evtmask 0x%08lx\n", success, err, evtmask);
    ok(success || err == ERROR_IO_PENDING, "overlapped WaitCommEvent failed\n");
    trace("overlapped WaitCommEvent returned.\n");
    if (!success && (err == ERROR_IO_PENDING))
	ok(WaitForSingleObjectEx(hComPortEvent, TIMEOUT, TRUE) == 0,
		     "wait hComPortEvent failed\n");
    success = GetOverlappedResult(hcom, &overlapped, &written, FALSE);
    err = GetLastError();
    after1 = GetTickCount();
    trace("Success 0x%08lx err %ld evtmask 0x%08lx diff1 %ld, diff2 %ld\n",
	  success, err, evtmask, after-before, after1-before);

    ok(evtmask & EV_RING, "Failed to detect  EV_RING: 0x%08lx, expected 0x%08x\n",
       evtmask, EV_RING);
    ok(GetCommModemStatus(hcom, &evtmask), "GetCommModemStatus failed\n");
    if(defaultStat & MS_RING_ON)
	ok((evtmask & MS_RING_ON) == 0,"DTR didn't change state!\n");
    else
	ok((evtmask & MS_RING_ON), "DTR didn't change state!\n");

    diff = after1 - before;
    ok ((diff > (TIMEOUT>>1) -TIMEDELTA) && (diff < (TIMEOUT>>1) + TIMEDELTA),
                  "Unexpected time %ld, expected around %d\n", diff, TIMEOUT>>1);

    /*restore RTS Settings*/
    if(defaultStat & MS_RING_ON)
	args[1] = SETDTR;
    else
	args[1] = CLRDTR;

    CloseHandle(hcom);
    ok( !WaitForSingleObject( alarmThread, 10000 ), "thread still running\n" );
    CloseHandle( alarmThread );
}
/*
 * Wait for a change in DCD
 * Needs Loopback from DTR to DCD
 */
static void test_WaitDcd(void)
{
    DCB dcb;
    OVERLAPPED overlapped;
    HANDLE hcom, hComPortEvent, alarmThread;
    DWORD_PTR args[3];
    DWORD alarmThreadId, before, after, after1, diff, success, err, written, evtmask=0, defaultStat = 0;

    if (!loopback_dtr_dcd) return;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(GetCommState(hcom, &dcb), "GetCommState failed\n");
    if (dcb.fDtrControl == DTR_CONTROL_DISABLE)
    {
	trace("DTR_CONTROL_HANDSHAKE is set, so don't manipulate DTR\n");
	CloseHandle(hcom);
	return;
    }
    args[0]= TIMEOUT >>1;
    ok(GetCommModemStatus(hcom, &defaultStat), "GetCommModemStatus failed\n");
    if(defaultStat & MS_RLSD_ON)
	args[1] = CLRDTR;
    else
	args[1] = SETDTR;
    args[2]= (DWORD_PTR)hcom;

    trace("test_WaitDcd timeout %Id clt 0x%08Ix handle %p\n",args[0], args[1], hcom);

    ok(SetCommMask(hcom, EV_RLSD), "SetCommMask failed\n");
    hComPortEvent =  CreateEventW( NULL, TRUE, FALSE, NULL );
    ok(hComPortEvent != 0, "CreateEvent failed\n");
    alarmThread = CreateThread(NULL, 0, toggle_ctlLine, args, 0, &alarmThreadId);
    ok(alarmThread !=0 , "CreateThread Failed\n");

    ZeroMemory( &overlapped, sizeof(overlapped));
    overlapped.hEvent = hComPortEvent;
    before = GetTickCount();
    success = WaitCommEvent(hcom, &evtmask, &overlapped);
    err = GetLastError();
    after = GetTickCount();

    trace("Success 0x%08lx err %ld evtmask 0x%08lx\n", success, err, evtmask);
    ok(success || err == ERROR_IO_PENDING, "overlapped WaitCommEvent failed\n");
    trace("overlapped WaitCommEvent returned.\n");
    if (!success && (err == ERROR_IO_PENDING))
	ok(WaitForSingleObjectEx(hComPortEvent, TIMEOUT, TRUE) == 0,
		     "wait hComPortEvent failed\n");
    success = GetOverlappedResult(hcom, &overlapped, &written, FALSE);
    err = GetLastError();
    after1 = GetTickCount();
    trace("Success 0x%08lx err %ld evtmask 0x%08lx diff1 %ld, diff2 %ld\n",
	  success, err, evtmask, after-before, after1-before);

    ok(evtmask & EV_RLSD, "Failed to detect  EV_RLSD: 0x%08lx, expected 0x%08x\n",
		 evtmask, EV_RLSD);
    ok(GetCommModemStatus(hcom, &evtmask), "GetCommModemStatus failed\n");
    if(defaultStat & MS_RLSD_ON)
	ok((evtmask & MS_RLSD_ON) == 0,"DTR didn't change state!\n");
    else
	ok((evtmask & MS_RLSD_ON), "DTR didn't change state!\n");

    diff = after1 - before;
    ok ((diff > (TIMEOUT>>1) -TIMEDELTA) && (diff < (TIMEOUT>>1) + TIMEDELTA),
                  "Unexpected time %ld, expected around %d\n", diff, TIMEOUT>>1);

    /*restore RTS Settings*/
    if(defaultStat & MS_RLSD_ON)
	args[1] = SETDTR;
    else
	args[1] = CLRDTR;

    CloseHandle(hcom);
    ok( !WaitForSingleObject( alarmThread, 10000 ), "thread still running\n" );
    CloseHandle( alarmThread );
}

/* 
   Set Break after timeout
*/
static DWORD CALLBACK set_CommBreak(LPVOID arg)
{
    DWORD_PTR *args = arg;
    DWORD timeout = args[0];
    HANDLE hcom   = (HANDLE) args[1];

    trace("SetCommBreak for handle %p after timeout %ld\n",
	  hcom, timeout);
    Sleep(timeout);
    ok(SetCommBreak(hcom),"SetCommBreak %p failed\n", hcom);
    trace("SetCommBreak done\n");
    return 0;
}

/*
   Wait for the Break condition (TX resp. RX active)
   Needs Loopback TX-RX
*/
static void test_WaitBreak(void)
{
    OVERLAPPED overlapped;
    HANDLE hcom, hComPortEvent, alarmThread;
    DWORD_PTR args[2];
    DWORD alarmThreadId, before, after, after1, diff, success, err, written, evtmask=0;

    if (!loopback_txd_rxd) return;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ok(SetCommMask(hcom, EV_BREAK), "SetCommMask failed\n");
    hComPortEvent =  CreateEventW( NULL, TRUE, FALSE, NULL );
    ok(hComPortEvent != 0, "CreateEvent failed\n");

    trace("test_WaitBreak\n");
    args[0]= TIMEOUT >>1;
    args[1]= (DWORD_PTR)hcom;
    alarmThread = CreateThread(NULL, 0, set_CommBreak, args, 0, &alarmThreadId);
    /* Wait a minimum to let the thread start up */
    Sleep(10);
    trace("Thread created\n");
    ok(alarmThread !=0 , "CreateThread Failed\n");

    ZeroMemory( &overlapped, sizeof(overlapped));
    overlapped.hEvent = hComPortEvent;
    before = GetTickCount();
    success = WaitCommEvent(hcom, &evtmask, &overlapped);
    err = GetLastError();
    after = GetTickCount();

    trace("Success 0x%08lx err %ld evtmask 0x%08lx\n", success, err, evtmask);
    ok(success || err == ERROR_IO_PENDING, "overlapped WaitCommEvent failed\n");
    trace("overlapped WaitCommEvent returned.\n");

    if (!success && (err == ERROR_IO_PENDING))
    {
        success = WaitForSingleObjectEx(hComPortEvent, TIMEOUT, TRUE);
        ok(!success, "wait hComPortEvent res %ld\n", GetLastError());
    }
    success = GetOverlappedResult(hcom, &overlapped, &written, FALSE);
    err = GetLastError();
    after1 = GetTickCount();
    trace("Success 0x%08lx err %ld evtmask 0x%08lx diff1 %ld, diff2 %ld\n",
	  success, err, evtmask, after-before, after1-before);

    ok(evtmask & EV_BREAK, "Failed to detect  EV_BREAK: 0x%08lx, expected 0x%08x\n",
       evtmask, EV_BREAK);
    ok(GetCommModemStatus(hcom, &evtmask), "GetCommModemStatus failed\n");

    diff = after1 - before;
    ok ((diff > (TIMEOUT>>1) -TIMEDELTA) && (diff < (TIMEOUT>>1) + TIMEDELTA),
	"Unexpected time %ld, expected around %d\n", diff, TIMEOUT>>1);

    ok(ClearCommBreak(hcom), "ClearCommBreak failed\n");

    CloseHandle(hcom);
    ok( !WaitForSingleObject( alarmThread, 10000 ), "thread still running\n" );
    CloseHandle( alarmThread );
}

static void test_stdio(void)
{
    DCB dcb;

    /* cygwin tries this to determine the stdin handle type */
    ok( !GetCommState( GetStdHandle(STD_INPUT_HANDLE), &dcb ), "GetCommState succeeded on stdin\n" );
    ok( GetLastError() == ERROR_INVALID_HANDLE || GetLastError() == ERROR_INVALID_FUNCTION,
        "got error %lu\n", GetLastError() );
}

static void test_WaitCommEvent(void)
{
    HANDLE hcom;
    DWORD evtmask, ret, bytes, before, after, last_event_time;
    OVERLAPPED ovl_wait;

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    test_GetModemStatus(hcom);

    ret = SetCommMask(hcom, 0x1fff);
    ok(ret, "SetCommMask error %ld\n", GetLastError());

    ovl_wait.Offset = 0;
    ovl_wait.OffsetHigh = 0;
    ovl_wait.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    trace("waiting 10 secs for com port events (turn on/off the device)...\n");
    last_event_time = 0;
    before = GetTickCount();
    do
    {
        evtmask = 0;
        SetLastError(0xdeadbeef);
        ret = WaitCommEvent(hcom, &evtmask, &ovl_wait);
        ok(!ret && GetLastError() == ERROR_IO_PENDING, "WaitCommEvent returned %ld, error %ld\n", ret, GetLastError());
        if (GetLastError() != ERROR_IO_PENDING) goto done; /* no point in further testing */
        for (;;)
        {
            ret = WaitForSingleObject(ovl_wait.hEvent, 500);
            after = GetTickCount();
            if (ret == WAIT_OBJECT_0)
            {
                last_event_time = after;
                ret = GetOverlappedResult(hcom, &ovl_wait, &bytes, FALSE);
                ok(ret, "GetOverlappedResult reported error %ld\n", GetLastError());
                ok(bytes == sizeof(evtmask), "expected %u, written %lu\n", (UINT)sizeof(evtmask), bytes);
                trace("WaitCommEvent: got events %#lx\n", evtmask);
                test_GetModemStatus(hcom);
                break;
            }
            else if (last_event_time || after - before >= 10000)
            {
                CancelIoEx(hcom, &ovl_wait);
                ret = WaitForSingleObject(ovl_wait.hEvent, 500);
                ok(ret == WAIT_OBJECT_0, "Wait for object failed\n");
                goto done;
            }
        }
    } while (after - before < 10000);

done:
    CloseHandle(ovl_wait.hEvent);
    CloseHandle(hcom);
}

static void test_FlushFileBuffers(void)
{
    HANDLE hcom;
    DWORD  ret, bytes, errors;
    COMSTAT stat;

    hcom = test_OpenComm(FALSE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ret = WriteFile(hcom, "\0\0\0\0\0\0\0", 7, &bytes, NULL);
    ok(ret, "WriteFile error %ld\n", GetLastError());
    ok(bytes == 7, "expected 7, got %lu\n", bytes);

    ret = FlushFileBuffers(hcom);
    ok(ret, "FlushFileBuffers error %ld\n", GetLastError());

    ret = ClearCommError(hcom, &errors, &stat);
    ok(ret, "ClearCommError error %ld\n", GetLastError());
    ok(stat.cbInQue == 0, "expected 0, got %ld bytes in InQueue\n", stat.cbInQue);
    ok(stat.cbOutQue == 0, "expected 0, got %ld bytes in OutQueue\n", stat.cbOutQue);
    ok(errors == 0, "expected errors 0, got %#lx\n", errors);

    CloseHandle(hcom);
}

static void test_read_write(void)
{
    static const char atz[]="ATZ\r\n";
    char buf[256];
    HANDLE hcom;
    DCB dcb;
    COMMTIMEOUTS timeouts;
    DWORD ret, bytes, status, evtmask, before, after, last_event_time;
    OVERLAPPED ovl_wait;
    IO_STATUS_BLOCK iob;
    LARGE_INTEGER offset;
    LONG i;

    if (!pNtReadFile || !pNtWriteFile)
    {
        win_skip("not running on NT, skipping test\n");
        return;
    }

    hcom = test_OpenComm(TRUE);
    if (hcom == INVALID_HANDLE_VALUE) return;

    ret = GetCommState(hcom, &dcb);
    ok(ret, "GetCommState error %ld\n", GetLastError());
    dcb.BaudRate = 9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.StopBits = ONESTOPBIT;
    ret = SetCommState(hcom, &dcb);
    ok(ret, "SetCommState error %ld\n", GetLastError());

    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadTotalTimeoutConstant = TIMEOUT;
    ret = SetCommTimeouts(hcom, &timeouts);
    ok(ret,"SetCommTimeouts error %ld\n", GetLastError());

    ret = SetupComm(hcom, 1024, 1024);
    ok(ret, "SetUpComm error %ld\n", GetLastError());

    bytes = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    ret = WriteFile(hcom, atz, 0, &bytes, NULL);
    ok(!ret, "WriteFile should fail\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %ld\n", GetLastError());
    ok(bytes == 0, "bytes %lu\n", bytes);

    iob.Status = -1;
    iob.Information = -1;
    status = pNtWriteFile(hcom, 0, NULL, NULL, &iob, atz, 0, NULL, NULL);
    ok(status == STATUS_INVALID_PARAMETER, "expected STATUS_INVALID_PARAMETER, got %#lx\n", status);
    ok(iob.Status == -1, "expected -1, got %#lx\n", iob.Status);
    ok(iob.Information == -1, "expected -1, got %Id\n", iob.Information);

    for (i = -20; i < 20; i++)
    {
        iob.Status = -1;
        iob.Information = -1;
        offset.QuadPart = (LONGLONG)i;
        status = pNtWriteFile(hcom, 0, NULL, NULL, &iob, atz, 0, &offset, NULL);
        if (i >= 0 || i == -1)
        {
            ok(status == STATUS_SUCCESS, "%ld: expected STATUS_SUCCESS, got %#lx\n", i, status);
            ok(iob.Status == STATUS_SUCCESS, "%ld: expected STATUS_SUCCESS, got %#lx\n", i, iob.Status);
            ok(iob.Information == 0, "%ld: expected 0, got %Iu\n", i, iob.Information);
        }
        else
        {
            ok(status == STATUS_INVALID_PARAMETER, "%ld: expected STATUS_INVALID_PARAMETER, got %#lx\n", i, status);
            ok(iob.Status == -1, "%ld: expected -1, got %#lx\n", i, iob.Status);
            ok(iob.Information == -1, "%ld: expected -1, got %Id\n", i, iob.Information);
        }
    }

    iob.Status = -1;
    iob.Information = -1;
    offset.QuadPart = 0;
    status = pNtWriteFile(hcom, 0, NULL, NULL, &iob, atz, sizeof(atz), &offset, NULL);
    ok(status == STATUS_PENDING || status == STATUS_SUCCESS, "expected STATUS_PENDING or STATUS_SUCCESS, got %#lx\n", status);
    /* Under Windows checking IO_STATUS_BLOCK right after the call leads
     * to races, iob.Status is either -1 or STATUS_SUCCESS, which means
     * that it's set only when the operation completes.
     */
    ret = WaitForSingleObject(hcom, TIMEOUT);
    if (ret == WAIT_TIMEOUT)
    {
        skip("Probably modem is not connected.\n");
        CloseHandle(hcom);
        return;
    }
    ok(ret == WAIT_OBJECT_0, "WaitForSingleObject error %ld\n", ret);
    ok(iob.Status == STATUS_SUCCESS, "expected STATUS_SUCCESS, got %#lx\n", iob.Status);
    ok(iob.Information == sizeof(atz), "expected sizeof(atz), got %Iu\n", iob.Information);

    ret = SetCommMask(hcom, EV_RXCHAR);
    ok(ret, "SetCommMask error %ld\n", GetLastError());

    ovl_wait.Offset = 0;
    ovl_wait.OffsetHigh = 0;
    ovl_wait.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    trace("waiting 3 secs for modem response...\n");
    last_event_time = 0;
    before = GetTickCount();
    do
    {
        evtmask = 0;
        SetLastError(0xdeadbeef);
        ret = WaitCommEvent(hcom, &evtmask, &ovl_wait);
        ok(!ret && GetLastError() == ERROR_IO_PENDING, "WaitCommEvent returned %ld, error %ld\n", ret, GetLastError());
        if (GetLastError() != ERROR_IO_PENDING) goto done; /* no point in further testing */
        for (;;)
        {
            ret = WaitForSingleObject(ovl_wait.hEvent, 100);
            after = GetTickCount();
            if (ret == WAIT_OBJECT_0)
            {
                trace("got modem response.\n");

                last_event_time = after;
                ret = GetOverlappedResult(hcom, &ovl_wait, &bytes, FALSE);
                ok(ret, "GetOverlappedResult reported error %ld\n", GetLastError());
                ok(bytes == sizeof(evtmask), "expected sizeof(evtmask), got %lu\n", bytes);
                ok(evtmask & EV_RXCHAR, "EV_RXCHAR should be set\n");

                bytes = 0xdeadbeef;
                SetLastError(0xdeadbeef);
                ret = ReadFile(hcom, buf, 0, &bytes, NULL);
                ok(!ret, "ReadFile should fail\n");
                ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %ld\n", GetLastError());
                ok(bytes == 0, "bytes %lu\n", bytes);

                iob.Status = -1;
                iob.Information = -1;
                status = pNtReadFile(hcom, 0, NULL, NULL, &iob, buf, 0, NULL, NULL);
                ok(status == STATUS_INVALID_PARAMETER, "expected STATUS_INVALID_PARAMETER, got %#lx\n", status);
                ok(iob.Status == -1, "expected -1, got %#lx\n", iob.Status);
                ok(iob.Information == -1, "expected -1, got %Id\n", iob.Information);

                for (i = -20; i < 20; i++)
                {
                    iob.Status = -1;
                    iob.Information = -1;
                    offset.QuadPart = (LONGLONG)i;
                    status = pNtReadFile(hcom, 0, NULL, NULL, &iob, buf, 0, &offset, NULL);
                    if (i >= 0)
                    {
                        ok(status == STATUS_SUCCESS, "%ld: expected STATUS_SUCCESS, got %#lx\n", i, status);
                        ok(iob.Status == STATUS_SUCCESS, "%ld: expected STATUS_SUCCESS, got %#lx\n", i, iob.Status);
                        ok(iob.Information == 0, "%ld: expected 0, got %Iu\n", i, iob.Information);
                    }
                    else
                    {
                        ok(status == STATUS_INVALID_PARAMETER, "%ld: expected STATUS_INVALID_PARAMETER, got %#lx\n", i, status);
                        ok(iob.Status == -1, "%ld: expected -1, got %#lx\n", i, iob.Status);
                        ok(iob.Information == -1, "%ld: expected -1, got %Id\n", i, iob.Information);
                    }
                }

                iob.Status = -1;
                iob.Information = -1;
                offset.QuadPart = 0;
                status = pNtReadFile(hcom, 0, NULL, NULL, &iob, buf, 1, &offset, NULL);
                ok(status == STATUS_SUCCESS, "expected STATUS_SUCCESS, got %#lx\n", status);
                ok(iob.Status == STATUS_SUCCESS, "expected STATUS_SUCCESS, got %#lx\n", iob.Status);
                ok(iob.Information == 1, "expected 1, got %Iu\n", iob.Information);
                goto done;
            }
            else if (last_event_time || after - before >= 3000)
            {
                CancelIoEx(hcom, &ovl_wait);
                ret = WaitForSingleObject(ovl_wait.hEvent, 100);
                ok(ret == WAIT_OBJECT_0, "Wait object failed\n");
                goto done;
            }
        }
    } while (after - before < 3000);

done:
    CloseHandle(ovl_wait.hEvent);
    CloseHandle(hcom);
}

#define MAX_COM_PORTS 256

static HKEY open_serialcomm(void)
{
    HKEY key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE | KEY_NOTIFY, &key))
        return NULL;
    return key;
}

static DWORD com_number(const WCHAR *name)
{
    WCHAR *end;
    DWORD n;

    n = wcsncmp(name, L"COM", 3) ? 0 : wcstoul(name + 3, &end, 10);
    return n && n <= MAX_COM_PORTS && !*end ? n : 0;
}

/* the COM numbers listed in SERIALCOMM, returns how many; check = test the values too */
static DWORD list_com_ports(HKEY key, BOOL *listed, BOOL check)
{
    DWORD i, n, count = 0, size, type, len;
    WCHAR name[64], value[16];
    LSTATUS ret;

    memset(listed, 0, (MAX_COM_PORTS + 1) * sizeof(*listed));
    for (i = 0;; i++)
    {
        len = ARRAY_SIZE(name);
        size = sizeof(value) - sizeof(WCHAR);
        memset(value, 0, sizeof(value));
        ret = RegEnumValueW(key, i, name, &len, NULL, &type, (BYTE *)value, &size);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (check) ok(!ret, "RegEnumValueW returned %ld.\n", ret);
        if (ret) break;
        if (check) ok(type == REG_SZ, "%s: got type %lu.\n", wine_dbgstr_w(name), type);
        if (!(n = com_number(value)))
        {
            if (check) trace("%s: skipping port %s.\n", wine_dbgstr_w(name), wine_dbgstr_w(value));
            continue;
        }
        if (check) ok(!listed[n], "COM%lu is listed twice.\n", n);
        listed[n] = TRUE;
        count++;
    }
    return count;
}

static BOOL comdb_claims(DWORD n)
{
    static BYTE comdb[4096 / 8];
    DWORD size = sizeof(comdb);

    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\COM Name Arbiter",
                     L"ComDB", RRF_RT_REG_BINARY, NULL, comdb, &size))
        return FALSE;
    return n <= size * 8 && (comdb[(n - 1) / 8] & (1 << ((n - 1) % 8)));
}

static void test_port_numbers(void)
{
    /* a distinct rate for each open port; 1200 is left out, it resets some boards */
    static const DWORD rates[] = { 600, 2400, 4800, 9600, 19200, 38400, 57600, 115200 };
    static HANDLE ports[MAX_COM_PORTS + 1];
    static DCB saved[MAX_COM_PORTS + 1];
    static DWORD expected[MAX_COM_PORTS + 1];
    static BOOL usable[MAX_COM_PORTS + 1];
    static BYTE comdb[4096 / 8];
    BOOL listed[MAX_COM_PORTS + 1];
    DWORD i, n, count, size;
    WCHAR path[16];
    HANDLE hcom;
    LSTATUS ret;
    BOOL res;
    HKEY key;
    DCB dcb;

    if (!(key = open_serialcomm()))
    {
        skip("No SERIALCOMM key.\n");
        return;
    }
    count = list_com_ports(key, listed, TRUE);
    RegCloseKey(key);
    if (!count)
    {
        skip("No COM port listed.\n");
        return;
    }

    /* every listed number is claimed in the COM port database */
    size = sizeof(comdb);
    ret = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\COM Name Arbiter",
                       L"ComDB", RRF_RT_REG_BINARY, NULL, comdb, &size);
    ok(!ret, "Can't read ComDB, error %ld.\n", ret);
    for (n = 1; !ret && n <= MAX_COM_PORTS; n++)
        if (listed[n])
            ok(n <= size * 8 && (comdb[(n - 1) / 8] & (1 << ((n - 1) % 8))),
               "COM%lu is not claimed in ComDB.\n", n);

    /* every listed port opens as a serial port */
    for (n = 1; n <= MAX_COM_PORTS; n++)
    {
        usable[n] = FALSE;
        if (!listed[n]) continue;
        swprintf(path, ARRAY_SIZE(path), L"\\\\.\\COM%lu", n);
        hcom = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hcom == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        {
            trace("COM%lu is in use.\n", n);
            continue;
        }
        ok(hcom != INVALID_HANDLE_VALUE, "Can't open listed COM%lu, error %lu.\n", n, GetLastError());
        if (hcom == INVALID_HANDLE_VALUE) continue;
        SetLastError(0xdeadbeef);
        usable[n] = GetCommState(hcom, &saved[n]);
        ok(usable[n], "Listed COM%lu is not a serial port, error %lu.\n", n, GetLastError());
        CloseHandle(hcom);
    }

    /* every port is a device of its own: it still opens while all the others are open... */
    for (n = 1; n <= MAX_COM_PORTS; n++)
    {
        ports[n] = INVALID_HANDLE_VALUE;
        if (!usable[n]) continue;
        swprintf(path, ARRAY_SIZE(path), L"\\\\.\\COM%lu", n);
        ports[n] = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        ok(ports[n] != INVALID_HANDLE_VALUE, "COM%lu doesn't open while the other ports are open, error %lu.\n",
           n, GetLastError());
    }

    /* ... and a rate set through one name doesn't show up through another one */
    for (n = 1, i = 0; n <= MAX_COM_PORTS; n++)
    {
        expected[n] = 0;
        if (ports[n] == INVALID_HANDLE_VALUE) continue;
        if (i == ARRAY_SIZE(rates))
        {
            trace("COM%lu: no distinct rate left.\n", n);
            continue;
        }
        dcb = saved[n];
        dcb.BaudRate = rates[i++];
        res = SetCommState(ports[n], &dcb);
        ok(res, "COM%lu: SetCommState failed, error %lu.\n", n, GetLastError());
        if (res) expected[n] = dcb.BaudRate;
    }
    for (n = 1; n <= MAX_COM_PORTS; n++)
    {
        if (!expected[n]) continue;
        res = GetCommState(ports[n], &dcb);
        ok(res, "COM%lu: GetCommState failed, error %lu.\n", n, GetLastError());
        ok(dcb.BaudRate == expected[n], "COM%lu: got rate %lu instead of %lu, another COM opens the same device.\n",
           n, dcb.BaudRate, expected[n]);
    }
    for (n = 1; n <= MAX_COM_PORTS; n++)
    {
        if (ports[n] == INVALID_HANDLE_VALUE) continue;
        SetCommState(ports[n], &saved[n]);
        CloseHandle(ports[n]);
    }

    /* a number that is not listed doesn't open anything */
    for (n = 1; n <= MAX_COM_PORTS; n++)
    {
        if (listed[n]) continue;
        swprintf(path, ARRAY_SIZE(path), L"\\\\.\\COM%lu", n);
        SetLastError(0xdeadbeef);
        hcom = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        ok(hcom == INVALID_HANDLE_VALUE, "Unlisted COM%lu opens.\n", n);
        if (hcom != INVALID_HANDLE_VALUE) CloseHandle(hcom);
        else ok(GetLastError() == ERROR_FILE_NOT_FOUND, "Unlisted COM%lu: got error %lu.\n", n, GetLastError());
    }
}

#define HOTPLUG_TIMEOUT 30000

static struct
{
    DWORD nodes_changed;    /* DBT_DEVNODES_CHANGED messages */
    BOOL nodes_listed[MAX_COM_PORTS + 1]; /* SERIALCOMM when the last one came */
    DWORD arrival, removal; /* port of the last DBT_DEVTYP_PORT message of each kind */
    BOOL arrival_listed, removal_listed; /* SERIALCOMM listed it when the message came */
} hotplug;

static LRESULT CALLBACK hotplug_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const DEV_BROADCAST_PORT_W *port = (const DEV_BROADCAST_PORT_W *)lparam;
    BOOL listed[MAX_COM_PORTS + 1];
    HKEY key;
    DWORD n;

    if (msg != WM_DEVICECHANGE) return DefWindowProcW(hwnd, msg, wparam, lparam);
    if (wparam == DBT_DEVNODES_CHANGED && (key = open_serialcomm()))
    {
        trace("DBT_DEVNODES_CHANGED.\n");
        hotplug.nodes_changed++;
        list_com_ports(key, hotplug.nodes_listed, FALSE);
        RegCloseKey(key);
    }
    if ((wparam != DBT_DEVICEARRIVAL && wparam != DBT_DEVICEREMOVECOMPLETE) || !port ||
        port->dbcp_devicetype != DBT_DEVTYP_PORT)
        return TRUE;
    trace("%s for %s.\n", wparam == DBT_DEVICEARRIVAL ? "DBT_DEVICEARRIVAL" : "DBT_DEVICEREMOVECOMPLETE",
          wine_dbgstr_w(port->dbcp_name));
    if (!(n = com_number(port->dbcp_name)) || !(key = open_serialcomm())) return TRUE;
    list_com_ports(key, listed, FALSE);
    RegCloseKey(key);
    if (wparam == DBT_DEVICEARRIVAL)
    {
        hotplug.arrival = n;
        hotplug.arrival_listed = listed[n];
    }
    else
    {
        hotplug.removal = n;
        hotplug.removal_listed = listed[n];
    }
    return TRUE;
}

static void pump_messages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
}

static void notify_serialcomm(HKEY key, HANDLE event)
{
    LSTATUS ret;

    ResetEvent(event);
    ret = RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, event, TRUE);
    ok(!ret, "RegNotifyChangeKeyValue returned %ld.\n", ret);
}

/* waits until a port is added to (> 0) or removed from (< 0) SERIALCOMM, 0 on timeout;
 * notified = a change notification came less than a second before the change was seen, or after it */
static int wait_port_change(HKEY key, HANDLE event, BOOL *listed, DWORD *last_notify, BOOL *notified)
{
    DWORD start = GetTickCount(), n, i, changed, seen;
    BOOL now[MAX_COM_PORTS + 1];

    *notified = FALSE;
    while (GetTickCount() - start < HOTPLUG_TIMEOUT)
    {
        if (!MsgWaitForMultipleObjects(1, &event, FALSE, 20, QS_ALLINPUT))
        {
            *last_notify = GetTickCount();
            notify_serialcomm(key, event);
        }
        pump_messages();
        list_com_ports(key, now, FALSE);
        for (n = 1; n <= MAX_COM_PORTS; n++)
        {
            if (now[n] == listed[n]) continue;
            for (i = n, changed = 0; i <= MAX_COM_PORTS; i++) changed += now[i] != listed[i];
            ok(changed == 1, "%lu ports changed at once.\n", changed);
            list_com_ports(key, now, TRUE);
            memcpy(listed, now, sizeof(now));
            seen = GetTickCount();
            if (seen - *last_notify > 1000 && !WaitForSingleObject(event, 1000))
            {
                *last_notify = GetTickCount();
                notify_serialcomm(key, event);
            }
            *notified = (int)(*last_notify - seen) >= -1000;
            return now[n] ? n : -(int)n;
        }
    }
    return 0;
}

static void wait_messages(DWORD timeout)
{
    DWORD start = GetTickCount();

    while (GetTickCount() - start < timeout)
    {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
        pump_messages();
    }
}

/* pumps messages until the DBT_DEVTYP_PORT message for the port came, and a DBT_DEVNODES_CHANGED
 * after SERIALCOMM changed */
static void wait_port_messages(DWORD *port, DWORD n, BOOL listed)
{
    DWORD start = GetTickCount();

    while (GetTickCount() - start < 5000 && (*port != n || hotplug.nodes_listed[n] != listed))
    {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
        pump_messages();
    }
}

static void test_port_hotplug(void)
{
    WNDCLASSW class = { .lpfnWndProc = hotplug_wndproc, .lpszClassName = L"comm_hotplug" };
    BOOL listed[MAX_COM_PORTS + 1], notified;
    DWORD i, n, removed = 0, last_notify;
    WCHAR path[16];
    HANDLE event, hcom;
    HWND hwnd;
    HKEY key;
    DCB dcb;
    int change;

    if (!(key = open_serialcomm()))
    {
        skip("No SERIALCOMM key.\n");
        return;
    }
    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    class.hInstance = GetModuleHandleW(NULL);
    RegisterClassW(&class);
    /* WM_DEVICECHANGE is broadcast to the top-level windows */
    hwnd = CreateWindowExW(0, class.lpszClassName, NULL, 0, 0, 0, 0, 0, NULL, NULL, class.hInstance, NULL);
    ok(hwnd != NULL, "CreateWindowExW failed, error %lu.\n", GetLastError());
    list_com_ports(key, listed, TRUE);
    /* let the messages sent before the window existed go by */
    wait_messages(1000);
    notify_serialcomm(key, event);
    last_notify = GetTickCount() - 10000;

    trace("Unplug a USB serial port and plug it back, or plug one in and unplug it, within %u seconds.\n",
          HOTPLUG_TIMEOUT / 1000);
    for (i = 0; i < 2; i++)
    {
        hotplug.nodes_changed = hotplug.arrival = hotplug.removal = 0;
        memcpy(hotplug.nodes_listed, listed, sizeof(listed));
        change = wait_port_change(key, event, listed, &last_notify, &notified);
        ok(change, "No COM port was added or removed in %u seconds.\n", HOTPLUG_TIMEOUT / 1000);
        if (!change)
        {
            ok(hotplug.nodes_changed, "Got no DBT_DEVNODES_CHANGED.\n");
            ok(!hotplug.arrival && !hotplug.removal, "Got a port message without a SERIALCOMM change.\n");
            continue;
        }

        n = abs(change);
        swprintf(path, ARRAY_SIZE(path), L"\\\\.\\COM%lu", n);
        if (change > 0)
        {
            trace("COM%lu was added.\n", n);
            ok(notified, "COM%lu was added without a SERIALCOMM change notification.\n", n);
            wait_port_messages(&hotplug.arrival, n, TRUE);
            ok(hotplug.arrival == n, "Got no DBT_DEVICEARRIVAL for COM%lu.\n", n);
            if (hotplug.arrival == n)
                ok(hotplug.arrival_listed, "DBT_DEVICEARRIVAL for COM%lu came before SERIALCOMM listed it.\n", n);
            ok(hotplug.nodes_listed[n], "Got no DBT_DEVNODES_CHANGED after COM%lu was listed.\n", n);
            ok(comdb_claims(n), "COM%lu is not claimed in ComDB.\n", n);
            if (removed)
                ok(n == removed, "The port came back as COM%lu instead of COM%lu.\n", n, removed);

            hcom = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            ok(hcom != INVALID_HANDLE_VALUE, "Can't open added COM%lu, error %lu.\n", n, GetLastError());
            if (hcom == INVALID_HANDLE_VALUE) continue;
            ok(GetCommState(hcom, &dcb), "Added COM%lu is not a serial port, error %lu.\n", n, GetLastError());
            CloseHandle(hcom);
        }
        else
        {
            trace("COM%lu was removed.\n", n);
            removed = n;
            ok(notified, "COM%lu was removed without a SERIALCOMM change notification.\n", n);
            wait_port_messages(&hotplug.removal, n, FALSE);
            ok(hotplug.removal == n, "Got no DBT_DEVICEREMOVECOMPLETE for COM%lu.\n", n);
            if (hotplug.removal == n)
                ok(!hotplug.removal_listed, "DBT_DEVICEREMOVECOMPLETE for COM%lu came while SERIALCOMM listed it.\n", n);
            ok(hotplug.nodes_changed && !hotplug.nodes_listed[n],
               "Got no DBT_DEVNODES_CHANGED after COM%lu was unlisted.\n", n);
            /* the number stays reserved for the device */
            ok(comdb_claims(n), "COM%lu is no longer claimed in ComDB.\n", n);

            SetLastError(0xdeadbeef);
            hcom = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            ok(hcom == INVALID_HANDLE_VALUE, "Removed COM%lu opens.\n", n);
            if (hcom != INVALID_HANDLE_VALUE) CloseHandle(hcom);
            else ok(GetLastError() == ERROR_FILE_NOT_FOUND, "Removed COM%lu: got error %lu.\n", n, GetLastError());
        }
    }

    DestroyWindow(hwnd);
    UnregisterClassW(class.lpszClassName, class.hInstance);
    CloseHandle(event);
    RegCloseKey(key);
}

#define BUSY_WINDOWS 5

/* like the window of a thread that is busy, e.g. waiting for a port to show up, when the port message comes */
static LRESULT CALLBACK busy_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_DEVICECHANGE && (wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE))
        Sleep(3000);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static DWORD CALLBACK busy_window_thread(void *ready)
{
    HWND hwnd;
    MSG msg;

    hwnd = CreateWindowExW(0, L"comm_hotplug_busy", NULL, 0, 0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(hwnd != NULL, "CreateWindowExW failed, error %lu.\n", GetLastError());
    SetEvent(ready);
    while (GetMessageW(&msg, 0, 0, 0)) DispatchMessageW(&msg);
    DestroyWindow(hwnd);
    return 0;
}

/* SERIALCOMM follows the devices while windows are slow to answer the port messages */
static void test_port_hotplug_busy(void)
{
    WNDCLASSW class = { .lpfnWndProc = hotplug_wndproc, .lpszClassName = L"comm_hotplug" };
    WNDCLASSW busy_class = { .lpfnWndProc = busy_wndproc, .lpszClassName = L"comm_hotplug_busy" };
    BOOL listed[MAX_COM_PORTS + 1], now[MAX_COM_PORTS + 1];
    DWORD i, n, start, count = 0, seen[2], tids[BUSY_WINDOWS];
    HANDLE threads[BUSY_WINDOWS], ready;
    HWND hwnd;
    HKEY key;

    if (!(key = open_serialcomm()))
    {
        skip("No SERIALCOMM key.\n");
        return;
    }
    class.hInstance = busy_class.hInstance = GetModuleHandleW(NULL);
    RegisterClassW(&class);
    RegisterClassW(&busy_class);
    /* this window comes after the busy ones in the z-order */
    hwnd = CreateWindowExW(0, class.lpszClassName, NULL, 0, 0, 0, 0, 0, NULL, NULL, class.hInstance, NULL);
    ok(hwnd != NULL, "CreateWindowExW failed, error %lu.\n", GetLastError());
    ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    for (i = 0; i < BUSY_WINDOWS; i++)
    {
        threads[i] = CreateThread(NULL, 0, busy_window_thread, ready, 0, &tids[i]);
        WaitForSingleObject(ready, 5000);
    }
    list_com_ports(key, listed, TRUE);
    wait_messages(1000);

    trace("Unplug a USB serial port and plug it back within 5 seconds.\n");
    start = GetTickCount();
    while (count < 2 && GetTickCount() - start < HOTPLUG_TIMEOUT)
    {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
        pump_messages();
        list_com_ports(key, now, FALSE);
        for (n = 1; n <= MAX_COM_PORTS; n++)
        {
            if (now[n] == listed[n]) continue;
            seen[count++] = GetTickCount();
            trace("COM%lu was %s.\n", n, now[n] ? "added" : "removed");
            break;
        }
        memcpy(listed, now, sizeof(now));
    }
    ok(count == 2, "Got %lu COM port changes in %u seconds.\n", count, HOTPLUG_TIMEOUT / 1000);
    if (count == 2)
        ok(seen[1] - seen[0] < 8000, "SERIALCOMM changed %lu ms after the previous change.\n", seen[1] - seen[0]);
    /* the port messages that are still on their way */
    wait_messages(5000);

    for (i = 0; i < BUSY_WINDOWS; i++) PostThreadMessageW(tids[i], WM_QUIT, 0, 0);
    WaitForMultipleObjects(BUSY_WINDOWS, threads, TRUE, 10000);
    for (i = 0; i < BUSY_WINDOWS; i++) CloseHandle(threads[i]);
    CloseHandle(ready);
    DestroyWindow(hwnd);
    UnregisterClassW(busy_class.lpszClassName, busy_class.hInstance);
    UnregisterClassW(class.lpszClassName, class.hInstance);
    RegCloseKey(key);
}

START_TEST(comm)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll)
    {
        pNtReadFile = (void *)GetProcAddress(ntdll, "NtReadFile");
        pNtWriteFile = (void *)GetProcAddress(ntdll, "NtWriteFile");
    }

    test_ClearCommError(); /* keep it the very first test */
    test_FlushFileBuffers();
    test_BuildCommDCB();
    test_ReadTimeOut();
    test_waittxempty();
    test_non_pending_errors();
    test_LoopbackRead();
    test_LoopbackCtsRts();
    test_LoopbackDtrDsr();
    test_LoopbackDtrRing();
    test_LoopbackDtrDcd();
    test_WaitRx();
    test_WaitRxHistory();
    test_WaitCts();
    test_AbortWaitCts();
    test_AbortWaitLoop();
    test_AbortWaitMask();
    test_CloseWaitPending();
    test_ReadOnlyWaitPending();
    test_CloseWaitDuplicate();
    test_WaitDsr();
    test_WaitRing();
    test_WaitDcd();
    test_WaitBreak();
    test_stdio();
    test_read_write();
    test_port_numbers();

    if (!winetest_interactive)
    {
        skip("interactive tests (set WINETEST_INTERACTIVE=1)\n");
        return;
    }

    test_WaitCommEvent();
    test_port_hotplug();
    test_port_hotplug_busy();
}
