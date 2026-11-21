/**
 * @file AppleFbUartSerialPortLib.c
 *
 * SerialPortLib that mirrors every byte to two sinks:
 *   1. the Samsung-style UART (same code as AppleUartSerialPortLib), and
 *   2. a text console drawn straight into the iBoot framebuffer.
 *
 * Why: on a machine with no serial adapter (any MacBook without a second
 * computer on the debug USB port) the firmware is otherwise completely
 * blind between m1n1's last line and the first GOP output from
 * SimpleFbDxe. With this library every DEBUG() in SEC, DXE core and every
 * driver lands on the built-in display.
 *
 * How: the framebuffer geometry comes from the boot_args copy that PrePi's
 * EarlySetup places at PcdBootArgsPointer (base, stride, width, height,
 * depth). Every module links its own copy of this library, so the cursor
 * lives in a small block at PcdBootArgsPointer + 0x2000 (boot_args itself
 * is under 2 KiB; the ADT copy starts at +0x4000) rather than in a global.
 * Before EarlySetup has run the block fails validation and only the UART
 * is used. The console wraps to the top instead of scrolling (a scroll is
 * a 24 MB memmove on a 3024x1964 panel), and blanks the line below the
 * cursor so the newest text is easy to find.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
 */
#include <PiDxe.h>
#include <Library/ArmLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/HobLib.h>
#include <Library/SerialPortLib.h>
#include <AArch64.h>
#include <Library/AppleUartSerialPortLib.h>
#include <Library/AppleDTLib.h>
#include <AppleFbConsole.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>

#include "Font8x16.h"

#define UART_BASE           FixedPcdGet64(PcdAppleUartBase)
#define BOOT_ARGS_COPY      ((struct boot_args *)(UINTN)FixedPcdGet64(PcdBootArgsPointer))
#define FB_CONSOLE_STATE    ((FB_CONSOLE *)((UINTN)FixedPcdGet64(PcdBootArgsPointer) + APPLE_FB_CONSOLE_OFFSET))
#define FB_CONSOLE_MAGIC    APPLE_FB_CONSOLE_MAGIC
/* Plain-text copy of everything printed, 1 MiB ring 16 MiB above boot_args
   (well clear of the ADT copy at +0x4000; PrePi reserves it). When an
   ASSERT/exception line goes by, the console switches to the small font and
   replays the tail of this ring, so a photo of the final screen shows ~4x
   more history than the live view. */
#define FB_LOG_BASE         ((UINT8 *)((UINTN)FixedPcdGet64(PcdBootArgsPointer) + 0x1000000))
#define FB_LOG_SIZE         0x100000u
#define FB_LINE_MAX         APPLE_FB_CONSOLE_LINE_MAX

#define FONT_W  8
#define FONT_H  16

typedef APPLE_FB_CONSOLE FB_CONSOLE;

/* ------------------------------------------------------------------------ */
/* Framebuffer console                                                       */
/* ------------------------------------------------------------------------ */

/**
  Zero a framebuffer range with plain aligned 64-bit stores. The framebuffer
  is mapped as Device memory once the MMU is on, and BaseMemoryLib's SetMem
  may use DC ZVA / unaligned accesses, which fault there.
**/
STATIC
VOID
FbZero (
  IN VOID   *Dst,
  IN UINTN  Bytes
  )
{
  volatile UINT64  *P = (volatile UINT64 *)Dst;
  UINTN            N  = Bytes / sizeof (UINT64);

  while (N-- > 0) {
    *P++ = 0;
  }
}

STATIC
BOOLEAN
FbArgsLookSane (
  IN CONST struct boot_args  *Ba
  )
{
  CONST struct boot_video  *V;
  UINT32                   Depth;

  if (FixedPcdGet64 (PcdBootArgsPointer) == 0) {
    return FALSE;
  }
  V     = &Ba->video;
  Depth = (UINT32)(V->depth & 0xff);
  if ((V->base == 0) || ((V->base & 0xfff) != 0) || (V->base < 0x800000000ULL)) {
    return FALSE;
  }
  if ((V->width < 320) || (V->width > 8192) || (V->height < 200) || (V->height > 8192)) {
    return FALSE;
  }
  if ((V->stride < V->width * 4) || (V->stride > 0x100000) || ((V->stride & 3) != 0)) {
    return FALSE;
  }
  if ((Depth != 30) && (Depth != 32)) {
    return FALSE;
  }
  return TRUE;
}

/**
  Return the shared console state, (re)initialising it from boot_args when
  it has not been set up yet. NULL when no usable framebuffer is described.
**/
STATIC
FB_CONSOLE *
FbConsole (
  VOID
  )
{
  FB_CONSOLE               *C;
  CONST struct boot_args   *Ba;
  UINT32                   Scale;

  Ba = BOOT_ARGS_COPY;
  if (!FbArgsLookSane (Ba)) {
    return NULL;
  }
  C = FB_CONSOLE_STATE;
  if ((C->Magic == FB_CONSOLE_MAGIC) && (C->Base == Ba->video.base) &&
      (C->Cols != 0) && (C->Rows != 0)) {
    return C;
  }

  Scale     = (Ba->video.width >= 2000) ? 2 : 1;
  C->Base   = Ba->video.base;
  C->Stride = Ba->video.stride;
  C->Width  = Ba->video.width;
  C->Height = Ba->video.height;
  C->Depth  = (UINT32)(Ba->video.depth & 0xff);
  C->Scale  = Scale;
  C->Cols   = (UINT32)(C->Width / (FONT_W * Scale));
  C->Rows   = (UINT32)(C->Height / (FONT_H * Scale));
  C->Col    = 0;
  C->Row    = 0;
  C->LogHead    = 0;
  C->LogWrapped = 0;
  C->Dumped     = 0;
  C->Quiet      = 0;
  C->LineLen    = 0;
  C->Magic  = FB_CONSOLE_MAGIC;

  /* Start from a clean, black screen (m1n1's logo would otherwise remain). */
  FbZero ((VOID *)(UINTN)C->Base, (UINTN)(C->Stride * C->Height));
  WriteBackDataCacheRange ((VOID *)(UINTN)C->Base, (UINTN)(C->Stride * C->Height));
  return C;
}

STATIC
UINT32
FbPixel (
  IN FB_CONSOLE  *C,
  IN UINT8       R,
  IN UINT8       G,
  IN UINT8       B
  )
{
  if (C->Depth == 32) {
    return ((UINT32)R << 16) | ((UINT32)G << 8) | B;          /* xRGB8888 */
  }
  return ((UINT32)B << 2) | ((UINT32)G << 12) | ((UINT32)R << 22); /* 10:10:10, as m1n1 */
}

/** Fill one text row (font-row-aligned band) with black. */
STATIC
VOID
FbClearRow (
  IN FB_CONSOLE  *C,
  IN UINT32      Row
  )
{
  UINTN  Bytes;
  VOID   *P;

  if (Row >= C->Rows) {
    return;
  }
  Bytes = (UINTN)C->Stride * FONT_H * C->Scale;
  P     = (VOID *)(UINTN)(C->Base + (UINT64)Row * Bytes);
  FbZero (P, Bytes);
  WriteBackDataCacheRange (P, Bytes);
}

STATIC
VOID
FbDrawGlyph (
  IN FB_CONSOLE  *C,
  IN CHAR8       Ch
  )
{
  CONST UINT8  *Glyph;
  UINT32       Fg;
  UINT32       Bg;
  UINT32       Scale;
  UINT32       Y;
  UINT32       X;
  UINT32       Sy;
  UINT32       Sx;
  UINT8        Bits;
  UINT32       *Line;
  UINTN        LineBytes;
  UINT8        *Cell;

  if ((Ch < 0x20) || (Ch > 0x7e)) {
    Ch = '?';
  }
  Glyph     = mFont8x16[Ch - 0x20];
  Scale     = C->Scale;
  Fg        = FbPixel (C, 0xe6, 0xe6, 0xe6);
  Bg        = FbPixel (C, 0x00, 0x00, 0x00);
  LineBytes = (UINTN)FONT_W * Scale * 4;
  Cell      = (UINT8 *)(UINTN)(C->Base
                + (UINT64)C->Row * FONT_H * Scale * C->Stride
                + (UINT64)C->Col * FONT_W * Scale * 4);

  for (Y = 0; Y < FONT_H; Y++) {
    Bits = Glyph[Y];
    for (Sy = 0; Sy < Scale; Sy++) {
      Line = (UINT32 *)(Cell + ((UINTN)Y * Scale + Sy) * C->Stride);
      for (X = 0; X < FONT_W; X++) {
        UINT32  Px = ((Bits << X) & 0x80) ? Fg : Bg;
        for (Sx = 0; Sx < Scale; Sx++) {
          Line[X * Scale + Sx] = Px;
        }
      }
      WriteBackDataCacheRange (Line, LineBytes);
    }
  }
}

STATIC
VOID
FbNewline (
  IN FB_CONSOLE  *C
  )
{
  C->Col = 0;
  C->Row++;
  if (C->Row >= C->Rows) {
    C->Row = 0;
  }
  FbClearRow (C, C->Row);
  /* Blank the following row too, so the newest line always has a gap under it. */
  FbClearRow (C, (C->Row + 1 < C->Rows) ? C->Row + 1 : 0);
}

STATIC VOID FbLogAppend (IN FB_CONSOLE *C, IN CHAR8 Ch);
STATIC VOID FbEndOfLine (IN FB_CONSOLE *C);

STATIC
VOID
FbPutChar (
  IN FB_CONSOLE  *C,
  IN CHAR8       Ch,
  IN BOOLEAN     Record
  )
{
  if (Record) {
    FbLogAppend (C, Ch);
    if ((Ch >= 0x20) && (Ch <= 0x7e) && (C->LineLen < FB_LINE_MAX)) {
      C->Line[C->LineLen++] = Ch;
    }
  }
  if (Record && C->Quiet) {
    /* Front page owns the panel: keep the ring and the fatal-line watch, skip drawing. */
    if (Ch == '\n') {
      FbEndOfLine (C);
    }
    return;
  }
  switch (Ch) {
    case '\n':
      FbNewline (C);
      if (Record) {
        FbEndOfLine (C);
      }
      return;
    case '\r':
      C->Col = 0;
      return;
    case '\t':
      do {
        FbPutChar (C, ' ', FALSE);
      } while ((C->Col % 4) != 0 && C->Col != 0);
      return;
    case '\b':
      if (C->Col > 0) {
        C->Col--;
      }
      return;
    default:
      break;
  }
  if (C->Col >= C->Cols) {
    FbNewline (C);
  }
  FbDrawGlyph (C, Ch);
  C->Col++;
}

STATIC
VOID
FbLogAppend (
  IN FB_CONSOLE  *C,
  IN CHAR8       Ch
  )
{
  FB_LOG_BASE[C->LogHead] = (UINT8)Ch;
  C->LogHead++;
  if (C->LogHead >= FB_LOG_SIZE) {
    C->LogHead    = 0;
    C->LogWrapped = 1;
  }
}

STATIC
BOOLEAN
FbLineStartsWith (
  IN FB_CONSOLE   *C,
  IN CONST CHAR8  *Prefix
  )
{
  UINTN  I;

  for (I = 0; Prefix[I] != '\0'; I++) {
    if ((I >= C->LineLen) || (C->Line[I] != Prefix[I])) {
      return FALSE;
    }
  }
  return TRUE;
}

/**
  Something fatal was just printed. Switch to the small font, wipe the screen
  and replay as much of the log ring as fits, so the final picture carries
  the history that led here instead of only the last screenful.
**/
STATIC
VOID
FbDenseDump (
  IN FB_CONSOLE  *C
  )
{
  UINT32  Avail;
  UINT32  Want;
  UINT32  Start;
  UINT32  Lines;
  UINT32  I;
  UINT32  Pos;

  C->Dumped = 1;
  C->Quiet  = 0;
  C->Scale  = 1;
  C->Cols   = (UINT32)(C->Width / FONT_W);
  C->Rows   = (UINT32)(C->Height / FONT_H);
  C->Col    = 0;
  C->Row    = 0;
  FbZero ((VOID *)(UINTN)C->Base, (UINTN)(C->Stride * C->Height));
  WriteBackDataCacheRange ((VOID *)(UINTN)C->Base, (UINTN)(C->Stride * C->Height));

  Avail = C->LogWrapped ? FB_LOG_SIZE : C->LogHead;
  /* Walk back at most Rows-2 newlines (leave room for what comes next). */
  Want  = 0;
  Lines = 0;
  Pos   = C->LogHead;
  while ((Want < Avail) && (Lines < C->Rows - 2)) {
    Pos = (Pos == 0) ? FB_LOG_SIZE - 1 : Pos - 1;
    Want++;
    if ((FB_LOG_BASE[Pos] == '\n') && (Want > 1)) {
      Lines++;
    }
  }
  Start = (C->LogHead + FB_LOG_SIZE - Want) % FB_LOG_SIZE;
  if (FB_LOG_BASE[Start] == '\n') {
    Start = (Start + 1) % FB_LOG_SIZE;
    Want--;
  }
  for (I = 0; I < Want; I++) {
    FbPutChar (C, (CHAR8)FB_LOG_BASE[(Start + I) % FB_LOG_SIZE], FALSE);
  }
}

/* Tiny local string helpers: this library sits under DebugLib, so it must not
   pull in BaseLib/BaseMemoryLib (which ASSERT through DebugLib). */
STATIC
CONST CHAR8 *
FbStrStr (
  IN CONST CHAR8  *Hay,
  IN CONST CHAR8  *Needle
  )
{
  UINTN  I;
  UINTN  J;

  if (Needle[0] == '\0') {
    return Hay;
  }
  for (I = 0; Hay[I] != '\0'; I++) {
    for (J = 0; Needle[J] != '\0' && Hay[I + J] == Needle[J]; J++) {
    }
    if (Needle[J] == '\0') {
      return Hay + I;
    }
  }
  return NULL;
}

/**
  "FBDUMP:kw1,kw2,...\n" went by: clear the panel, switch to the small font
  and replay, oldest first, every line in the log ring that contains one of
  the keywords. Unlike the fatal-line replay this walks the whole ring, so it
  reaches back to PrePi. BDS uses it to show the RAM disk / boot option trail
  when nothing was bootable.
**/
STATIC
VOID
FbFilteredDump (
  IN FB_CONSOLE  *C
  )
{
  CHAR8   Keys[FB_LINE_MAX];
  CHAR8   Line[FB_LINE_MAX + 1];
  UINTN   KeyLen;
  UINT32  Avail;
  UINT32  Start;
  UINT32  I;
  UINTN   Len;
  BOOLEAN Overflow;

  KeyLen = C->LineLen - 7;                         /* strip "FBDUMP:" */
  if ((KeyLen == 0) || (KeyLen >= sizeof (Keys))) {
    return;
  }
  for (I = 0; I < KeyLen; I++) {
    Keys[I] = C->Line[7 + I];
  }
  Keys[KeyLen] = '\0';

  C->Quiet = 0;
  C->Scale = 1;
  C->Cols  = (UINT32)(C->Width / FONT_W);
  C->Rows  = (UINT32)(C->Height / FONT_H);
  C->Col   = 0;
  C->Row   = 0;
  FbZero ((VOID *)(UINTN)C->Base, (UINTN)(C->Stride * C->Height));
  WriteBackDataCacheRange ((VOID *)(UINTN)C->Base, (UINTN)(C->Stride * C->Height));

  Avail = C->LogWrapped ? FB_LOG_SIZE : C->LogHead;
  Start = C->LogWrapped ? C->LogHead : 0;
  Len   = 0;
  Overflow = FALSE;
  for (I = 0; I < Avail; I++) {
    CHAR8  Ch = (CHAR8)FB_LOG_BASE[(Start + I) % FB_LOG_SIZE];
    if (Ch != '\n') {
      if (Len < FB_LINE_MAX) {
        Line[Len++] = Ch;
      } else {
        Overflow = TRUE;
      }
      continue;
    }
    Line[Len] = '\0';
    if ((Len > 0) && (FbStrStr (Line, "FBDUMP:") != Line)) {
      CONST CHAR8  *K = Keys;
      while (*K != '\0') {
        CONST CHAR8  *Comma = FbStrStr (K, ",");
        UINTN        KLen;
        UINTN        M;
        CHAR8        Kw[64];
        for (KLen = 0; K[KLen] != '\0' && K[KLen] != ','; KLen++) {
        }
        if ((KLen > 0) && (KLen < sizeof (Kw))) {
          for (M = 0; M < KLen; M++) {
            Kw[M] = K[M];
          }
          Kw[KLen] = '\0';
          if (FbStrStr (Line, Kw) != NULL) {
            UINTN  J;
            for (J = 0; J < Len; J++) {
              FbPutChar (C, Line[J], FALSE);
            }
            FbPutChar (C, Overflow ? '~' : ' ', FALSE);
            FbPutChar (C, '\n', FALSE);
            break;
          }
        }
        K = (Comma != NULL) ? Comma + 1 : K + KLen;
      }
    }
    Len = 0;
    Overflow = FALSE;
  }
}

STATIC
VOID
FbEndOfLine (
  IN FB_CONSOLE  *C
  )
{
  BOOLEAN  Fatal;

  Fatal = FbLineStartsWith (C, "ASSERT") ||
          FbLineStartsWith (C, "ASSERT_EFI_ERROR") ||
          FbLineStartsWith (C, "Synchronous Exception") ||
          FbLineStartsWith (C, "SError") ||
          FbLineStartsWith (C, "Data abort") ||
          FbLineStartsWith (C, "Instruction abort") ||
          FbLineStartsWith (C, "IRQ Exception") ||
          FbLineStartsWith (C, "FIQ Exception") ||
          FbLineStartsWith (C, "!!!! ") ||
          FbLineStartsWith (C, "PrePi: Memory setup failed");
  if (FbLineStartsWith (C, "FBDUMP:")) {
    FbFilteredDump (C);
    C->LineLen = 0;
    return;
  }
  C->LineLen = 0;
  if (Fatal && !C->Dumped) {
    FbDenseDump (C);
  }
}

/* ------------------------------------------------------------------------ */
/* SerialPortLib entry points                                                */
/* ------------------------------------------------------------------------ */

EFI_STATUS EFIAPI SerialPortInitialize(VOID)
{
    UINT32 BaudRateConfig = AppleSerialPortCalculateBaudRateConfig();
    SerialPortFlush();
    MmioWrite32((UART_BASE + UART_BAUD_RATE_CONFIG), BaudRateConfig);
    (VOID)FbConsole ();          /* harmless before EarlySetup: returns NULL */
    return EFI_SUCCESS;
}

UINTN EFIAPI SerialPortWrite(IN UINT8 *Buffer, IN UINTN NumberOfBytes)
{
    UINTN       NumberOfBytesToTransmit = NumberOfBytes;
    FB_CONSOLE  *C;

    ArmDisableInterrupts();
    for(UINTN i = 0; i < NumberOfBytesToTransmit; i++)
    {
        while(!(MmioRead32(UART_BASE + UART_TRANSFER_STATUS) & UART_TRANSFER_STATUS_TXBE))
        {
        }
        MmioWrite32((UART_BASE + UART_TX_BYTE), Buffer[i]);
        NumberOfBytes--;
    }
    C = FbConsole ();
    if (C != NULL) {
        for (UINTN i = 0; i < NumberOfBytesToTransmit; i++) {
            FbPutChar (C, (CHAR8)Buffer[i], TRUE);
        }
    }
    ArmEnableInterrupts();
    return NumberOfBytesToTransmit - NumberOfBytes;
}

/* The remaining entry points are unchanged from AppleUartSerialPortLib. */
UINTN EFIAPI SerialPortRead(
    OUT UINT8 *Buffer, 
    IN UINTN NumberOfBytes
    )
{
    UINTN  Count;
    for (Count = 0; (Count < NumberOfBytes) && SerialPortPoll (); Count++, Buffer++) {
      *Buffer = MmioRead32 (UART_BASE + UART_RX_BYTE);
    }
    return Count;
}

/*
   This function will perform the calculations needed to change the baud rate to 115200 
   (This can all be done inline but separating this logic out into a function makes it more readable.)
   If you want a different baud rate, change BaudRate to the desired baud rate.
*/
UINT32 AppleSerialPortCalculateBaudRateConfig(VOID)
{
    UINTN BaudRate = 115200;
    return (((UART_CLOCK / BaudRate + 7) / 16) - 1);
}

UINTN SerialPortFlush(VOID)
{
    while(!(MmioRead32(UART_BASE + UART_TRANSFER_STATUS) & UART_TRANSFER_STATUS_TXE))
    {

    }
    return 0;
}

/**
  Polls a serial device to see if there is any data waiting to be read.

  Polls a serial device to see if there is any data waiting to be read.
  If there is data waiting to be read from the serial device, then TRUE is returned.
  If there is no data waiting to be read from the serial device, then FALSE is returned.

  @retval TRUE             Data is waiting to be read from the serial device.
  @retval FALSE            There is no data waiting to be read from the serial device.

**/

BOOLEAN EFIAPI SerialPortPoll(VOID)
{
    return (MmioRead32(UART_BASE + UART_TRANSFER_STATUS) & UART_TRANSFER_STATUS_RXD) ? TRUE : FALSE;
}

/**
  Retrieve the status of the control bits on a serial device.

  @param Control                A pointer to return the current control signals from the serial device.

  @retval RETURN_SUCCESS        The control bits were read from the serial device.
  @retval RETURN_UNSUPPORTED    The serial device does not support this operation.
  @retval RETURN_DEVICE_ERROR   The serial device is not functioning correctly.

**/

RETURN_STATUS EFIAPI SerialPortGetControl(OUT UINT32 *Control)
{
    return RETURN_UNSUPPORTED;
}

/**
  Sets the control bits on a serial device.

  @param Control                Sets the bits of Control that are settable.

  @retval RETURN_SUCCESS        The new control bits were set on the serial device.
  @retval RETURN_UNSUPPORTED    The serial device does not support this operation.
  @retval RETURN_DEVICE_ERROR   The serial device is not functioning correctly.

**/

RETURN_STATUS EFIAPI SerialPortSetControl(IN UINT32 Control)
{
    return RETURN_UNSUPPORTED;
}


/**
  Sets the baud rate, receive FIFO depth, transmit/receice time out, parity,
  data bits, and stop bits on a serial device.

  @param BaudRate           The requested baud rate. A BaudRate value of 0 will use the
                            device's default interface speed.
                            On output, the value actually set.
  @param ReveiveFifoDepth   The requested depth of the FIFO on the receive side of the
                            serial interface. A ReceiveFifoDepth value of 0 will use
                            the device's default FIFO depth.
                            On output, the value actually set.
  @param Timeout            The requested time out for a single character in microseconds.
                            This timeout applies to both the transmit and receive side of the
                            interface. A Timeout value of 0 will use the device's default time
                            out value.
                            On output, the value actually set.
  @param Parity             The type of parity to use on this serial device. A Parity value of
                            DefaultParity will use the device's default parity value.
                            On output, the value actually set.
  @param DataBits           The number of data bits to use on the serial device. A DataBits
                            vaule of 0 will use the device's default data bit setting.
                            On output, the value actually set.
  @param StopBits           The number of stop bits to use on this serial device. A StopBits
                            value of DefaultStopBits will use the device's default number of
                            stop bits.
                            On output, the value actually set.

  @retval RETURN_SUCCESS            The new attributes were set on the serial device.
  @retval RETURN_UNSUPPORTED        The serial device does not support this operation.
  @retval RETURN_INVALID_PARAMETER  One or more of the attributes has an unsupported value.
  @retval RETURN_DEVICE_ERROR       The serial device is not functioning correctly.

**/
RETURN_STATUS EFIAPI SerialPortSetAttributes(
    IN OUT UINT64 *BaudRate,
    IN OUT UINT32 *ReceiveFifoDepth,
    IN OUT UINT32 *Timeout,
    IN OUT EFI_PARITY_TYPE *Parity,
    IN OUT UINT8 *DataBits,
    IN OUT EFI_STOP_BITS_TYPE *StopBits
)
{
    return RETURN_UNSUPPORTED;

}
