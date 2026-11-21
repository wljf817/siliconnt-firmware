/**
 * @file M1n1InitrdRamdiskDxe.c
 *
 * Expose the blob m1n1 loaded as an "initramfs" (see Guid/M1n1Initrd.h) as
 * a UEFI RAM disk, so the boot manager can start a WinPE / Windows image
 * that was simply concatenated onto m1n1's boot.bin on the ESP.
 *
 * Companion to BootRamdiskHelperDxe, which does the same for an image
 * embedded in the firmware volume; this one keeps the firmware small and
 * lets the image be swapped without rebuilding UEFI.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
 */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/RamDisk.h>
#include <Guid/M1n1Initrd.h>

// ISO 9660 primary volume descriptor: sector 16, "CD001" at byte 1.
#define ISO9660_PVD_OFFSET  0x8001
#define ISO9660_ID          "CD001"

EFI_STATUS
EFIAPI
M1n1InitrdRamdiskDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                Status;
  EFI_HOB_GUID_TYPE         *GuidHob;
  M1N1_INITRD_HOB           *Initrd;
  EFI_RAM_DISK_PROTOCOL     *RamDisk;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_GUID                  *DiskType;
  CONST UINT8               *Image;

  GuidHob = GetFirstGuidHob (&gAppleSiliconPkgM1n1InitrdHobGuid);
  if (GuidHob == NULL) {
    DEBUG ((DEBUG_INFO, "M1n1InitrdRamdiskDxe: no m1n1 initramfs HOB, nothing to do\n"));
    return EFI_NOT_FOUND;
  }
  Initrd = GET_GUID_HOB_DATA (GuidHob);
  if ((Initrd->Base == 0) || (Initrd->Size < 0x10000)) {
    DEBUG ((DEBUG_ERROR, "M1n1InitrdRamdiskDxe: bogus initramfs 0x%llx+0x%llx\n",
            Initrd->Base, Initrd->Size));
    return EFI_NOT_FOUND;
  }

  Status = gBS->LocateProtocol (&gEfiRamDiskProtocolGuid, NULL, (VOID **)&RamDisk);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "M1n1InitrdRamdiskDxe: RAM disk protocol missing - %r\n", Status));
    return Status;
  }

  // Register in place: PrePi already reserved the pages, so nothing else
  // will reuse them and we avoid a copy of a few hundred MiB.
  Image    = (CONST UINT8 *)(UINTN)Initrd->Base;
  DiskType = &gEfiVirtualDiskGuid;
  if ((Initrd->Size > ISO9660_PVD_OFFSET + 5) &&
      (CompareMem (Image + ISO9660_PVD_OFFSET, ISO9660_ID, 5) == 0)) {
    DiskType = &gEfiVirtualCdGuid;
  }

  Status = RamDisk->Register (Initrd->Base, Initrd->Size, DiskType, NULL, &DevicePath);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "M1n1InitrdRamdiskDxe: Register failed - %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "M1n1InitrdRamdiskDxe: registered %a RAM disk 0x%llx (%llu MiB)\n",
          (DiskType == &gEfiVirtualCdGuid) ? "ISO" : "raw", Initrd->Base, Initrd->Size >> 20));
  return EFI_SUCCESS;
}
