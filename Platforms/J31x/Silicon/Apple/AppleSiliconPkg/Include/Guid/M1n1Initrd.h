/**
 * @file M1n1Initrd.h
 *
 * HOB describing an "initramfs" payload that m1n1 loaded into RAM for us.
 *
 * m1n1 accepts arbitrary blobs appended to its stage-2 image (prefixed with
 * the "m1n1_initramfs" magic + u32 size) and advertises them to the next
 * stage through /chosen/linux,initrd-start and /chosen/linux,initrd-end in
 * the device tree it passes in x0. PrePi records that region in this HOB and
 * reserves it; M1n1InitrdRamdiskDxe later exposes it as a UEFI RAM disk so
 * the Windows boot manager can boot a WinPE/Windows image that lives next to
 * boot.bin on the ESP — no USB media, and nothing baked into the firmware
 * volume.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
 */
#ifndef M1N1_INITRD_H_
#define M1N1_INITRD_H_

#define M1N1_INITRD_HOB_GUID \
  { 0x3e0c6c53, 0x9d2a, 0x4f6b, { 0x9b, 0x1f, 0x5a, 0x77, 0x0c, 0x21, 0xe4, 0x8d } }

typedef struct {
  UINT64  Base;   ///< Physical address of the payload (page aligned by m1n1).
  UINT64  Size;   ///< Payload size in bytes.
} M1N1_INITRD_HOB;

extern EFI_GUID  gAppleSiliconPkgM1n1InitrdHobGuid;

#endif
