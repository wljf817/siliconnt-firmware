/** @file
  Reset the SoC through the primary watchdog.

  Register layout is the one m1n1 uses (src/wdt.c): COUNT @+0x10, ALARM @+0x14,
  CTL @+0x1c; CTL=4 arms the alarm as a reset, COUNT=0 restarts the counter
  and ALARM sets how far it counts before firing (~5 ms at 24 MHz here).

  Shutdown is not possible from here: powering off goes through the SMC
  coprocessor, which this firmware does not drive yet, so it is mapped to a
  reboot rather than leaving the machine hung with a black screen.

  BASE library on purpose (Mu links HwResetSystemLib into the DXE core too):
  it pokes the physical address straight from the PCD, so it works from the
  DXE core through BDS. After SetVirtualAddressMap the OS's own reset path is
  expected to take over; runtime ResetSystem is not supported yet.

  SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
**/
#include <Base.h>
#include <Uefi/UefiBaseType.h>
#include <Uefi/UefiSpec.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/ResetSystemLib.h>

#define WDT_COUNT  0x10
#define WDT_ALARM  0x14
#define WDT_CTL    0x1c

STATIC
VOID
WdtReboot (
  VOID
  )
{
  UINTN  WdtBase;

  WdtBase = (UINTN)PcdGet64 (PcdAppleWdtBase);
  if (WdtBase == 0) {
    DEBUG ((DEBUG_ERROR, "AppleWdtResetSystemLib: PcdAppleWdtBase unset, cannot reset\n"));
    return;
  }
  MmioWrite32 (WdtBase + WDT_ALARM, 0x100000);
  MmioWrite32 (WdtBase + WDT_COUNT, 0);
  MmioWrite32 (WdtBase + WDT_CTL, 4);
  // The watchdog fires a few milliseconds from now; don't return into the caller.
  CpuDeadLoop ();
}

VOID
EFIAPI
ResetCold (
  VOID
  )
{
  WdtReboot ();
}

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  WdtReboot ();
}

VOID
EFIAPI
ResetShutdown (
  VOID
  )
{
  DEBUG ((DEBUG_WARN, "AppleWdtResetSystemLib: no power-off path (needs SMC), rebooting instead\n"));
  WdtReboot ();
}

VOID
EFIAPI
ResetPlatformSpecific (
  IN UINTN  DataSize,
  IN VOID   *ResetData
  )
{
  WdtReboot ();
}

VOID
EFIAPI
ResetSystem (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  switch (ResetType) {
    case EfiResetShutdown:
      ResetShutdown ();
      break;
    case EfiResetPlatformSpecific:
      ResetPlatformSpecific (DataSize, ResetData);
      break;
    default:
      ResetCold ();
      break;
  }
}
