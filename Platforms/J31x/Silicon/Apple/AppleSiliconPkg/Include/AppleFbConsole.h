/** @file
  Layout of the framebuffer debug console state that AppleFbUartSerialPortLib
  keeps at PcdBootArgsPointer + 0x2000, shared by every module (SEC through
  BDS) so the cursor survives across images.

  Other modules only ever touch it through AppleFbConsoleSetQuiet(): BDS uses
  it to keep the DEBUG stream off the panel while the front page owns the
  screen. Everything printed still lands in the log ring, and a fatal line
  still triggers the dense replay.

  SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
**/
#ifndef APPLE_FB_CONSOLE_H_
#define APPLE_FB_CONSOLE_H_

#define APPLE_FB_CONSOLE_OFFSET    0x2000
#define APPLE_FB_CONSOLE_MAGIC     0x4E43424Fu   /* 'OBCN' */
#define APPLE_FB_CONSOLE_LINE_MAX  200

typedef struct {
  UINT32  Magic;
  UINT32  Col;
  UINT32  Row;
  UINT32  Scale;       /* 1 on low-DPI panels, 2 on Retina */
  UINT32  Cols;
  UINT32  Rows;
  UINT32  Depth;       /* 30 or 32 */
  UINT32  Quiet;       /* non-zero: log to the ring only, don't draw */
  UINT64  Base;
  UINT64  Stride;      /* bytes */
  UINT64  Width;
  UINT64  Height;
  UINT32  LogHead;     /* next write offset into the log ring */
  UINT32  LogWrapped;  /* ring has wrapped at least once */
  UINT32  Dumped;      /* already switched to dense mode */
  UINT32  LineLen;
  CHAR8   Line[APPLE_FB_CONSOLE_LINE_MAX];
} APPLE_FB_CONSOLE;

/**
  Stop (Quiet=TRUE) or resume (Quiet=FALSE) drawing the debug console on the
  panel. No-op when the console was never brought up (plain UART builds, or
  PcdBootArgsPointer unset).
**/
STATIC
inline
VOID
AppleFbConsoleSetQuiet (
  IN UINT64   BootArgsPointer,
  IN BOOLEAN  Quiet
  )
{
  volatile APPLE_FB_CONSOLE  *C;

  if (BootArgsPointer == 0) {
    return;
  }
  C = (volatile APPLE_FB_CONSOLE *)(UINTN)(BootArgsPointer + APPLE_FB_CONSOLE_OFFSET);
  if (C->Magic == APPLE_FB_CONSOLE_MAGIC) {
    C->Quiet = Quiet ? 1 : 0;
  }
}

#endif
