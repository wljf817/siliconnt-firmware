/**
 * @file M1n1Initrd.c
 *
 * Find the initramfs region m1n1 advertised in the flattened device tree,
 * reserve it so DXE never hands it out, and publish it in a HOB for
 * M1n1InitrdRamdiskDxe.
 *
 * The device tree is parsed by hand: PrePi has no FdtLib dependency and we
 * only need two properties from one node.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
 */
#include <PiPei.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Guid/M1n1Initrd.h>
#include <Library/PcdLib.h>
#include <Library/AppleDTLib.h>

#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  0x1
#define FDT_END_NODE    0x2
#define FDT_PROP        0x3
#define FDT_NOP         0x4
#define FDT_END         0x9

typedef struct {
  UINT32  Magic;
  UINT32  TotalSize;
  UINT32  OffDtStruct;
  UINT32  OffDtStrings;
  UINT32  OffMemRsvMap;
  UINT32  Version;
  UINT32  LastCompVersion;
  UINT32  BootCpuidPhys;
  UINT32  SizeDtStrings;
  UINT32  SizeDtStruct;
} FDT_HEADER;

STATIC
UINT32
Be32 (
  IN CONST VOID  *P
  )
{
  return SwapBytes32 (ReadUnaligned32 ((CONST UINT32 *)P));
}

/** Read a /chosen cell property that Linux allows as either u32 or u64. */
STATIC
UINT64
ReadCells (
  IN CONST UINT8  *Data,
  IN UINT32       Len
  )
{
  if (Len == sizeof (UINT64)) {
    return SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)Data));
  }
  if (Len == sizeof (UINT32)) {
    return Be32 (Data);
  }
  return 0;
}

/**
  Walk the FDT structure block looking for /chosen/linux,initrd-{start,end}.

  @retval TRUE   both properties were found and describe a non-empty region.
**/
STATIC
BOOLEAN
FdtFindInitrd (
  IN  CONST VOID  *Fdt,
  OUT UINT64      *Start,
  OUT UINT64      *End
  )
{
  CONST FDT_HEADER  *Hdr;
  CONST UINT8       *Struct;
  CONST UINT8       *StructEnd;
  CONST CHAR8       *Strings;
  CONST UINT8       *P;
  UINTN             Depth;
  UINTN             ChosenDepth;
  BOOLEAN           InChosen;
  BOOLEAN           HaveStart;
  BOOLEAN           HaveEnd;

  if (Fdt == NULL) {
    DEBUG ((DEBUG_INFO, "M1n1Initrd: no FDT pointer\n"));
    return FALSE;
  }
  Hdr = (CONST FDT_HEADER *)Fdt;
  DEBUG ((DEBUG_INFO, "M1n1Initrd: FDT at %p, magic 0x%08x, size %u\n",
          Fdt, Be32 (&Hdr->Magic), Be32 (&Hdr->TotalSize)));
  if (Be32 (&Hdr->Magic) != FDT_MAGIC) {
    return FALSE;
  }

  Struct    = (CONST UINT8 *)Fdt + Be32 (&Hdr->OffDtStruct);
  StructEnd = Struct + Be32 (&Hdr->SizeDtStruct);
  Strings   = (CONST CHAR8 *)Fdt + Be32 (&Hdr->OffDtStrings);
  if (StructEnd > (CONST UINT8 *)Fdt + Be32 (&Hdr->TotalSize)) {
    return FALSE;
  }

  P           = Struct;
  Depth       = 0;
  ChosenDepth = 0;
  InChosen    = FALSE;
  HaveStart = FALSE;
  HaveEnd   = FALSE;

  while (P + 4 <= StructEnd) {
    UINT32  Token = Be32 (P);
    P += 4;

    switch (Token) {
      case FDT_BEGIN_NODE: {
        CONST CHAR8  *Name = (CONST CHAR8 *)P;
        UINTN        NameLen = AsciiStrnLenS (Name, StructEnd - P);
        P += (NameLen + 1 + 3) & ~(UINTN)3;
        Depth++;
        // /chosen is a direct child of the root node (depth 2 here). Properties
        // always precede child nodes in the structure block, but keep the depth
        // bookkeeping exact anyway.
        if ((Depth == 2) && (AsciiStrCmp (Name, "chosen") == 0)) {
          ChosenDepth = Depth;
        }
        InChosen = (ChosenDepth != 0) && (Depth == ChosenDepth);
        break;
      }
      case FDT_END_NODE:
        if (Depth == ChosenDepth) {
          ChosenDepth = 0;            // leaving /chosen
        }
        if (Depth > 0) {
          Depth--;
        }
        InChosen = (ChosenDepth != 0) && (Depth == ChosenDepth);
        break;
      case FDT_PROP: {
        UINT32       Len     = Be32 (P);
        UINT32       NameOff = Be32 (P + 4);
        CONST UINT8  *Data   = P + 8;
        CONST CHAR8  *Name   = Strings + NameOff;
        P = Data + ((Len + 3) & ~(UINT32)3);
        if (!InChosen) {
          break;
        }
        if (AsciiStrCmp (Name, "linux,initrd-start") == 0) {
          *Start    = ReadCells (Data, Len);
          HaveStart = TRUE;
        } else if (AsciiStrCmp (Name, "linux,initrd-end") == 0) {
          *End    = ReadCells (Data, Len);
          HaveEnd = TRUE;
        }
        break;
      }
      case FDT_NOP:
        break;
      case FDT_END:
      default:
        P = StructEnd;
        break;
    }
    if (HaveStart && HaveEnd) {
      break;
    }
  }

  return HaveStart && HaveEnd && (*End > *Start);
}

/**
  Reserve the m1n1 initramfs region and describe it in a HOB. Silent no-op
  when m1n1 did not append one (the normal case for a plain firmware boot).

  Must run after MemoryPeim() so the region falls inside a system-memory
  resource descriptor, and before DXE core starts allocating.
**/
VOID
EFIAPI
M1n1InitrdSetup (
  IN CONST VOID  *DeviceTreePtr
  )
{
  UINT64           Start;
  UINT64           End;
  UINT64           Base;
  UINT64           Size;
  M1N1_INITRD_HOB  Hob;

  //
  // The FDT's memory reservation block (/memreserve/): m1n1 puts its own image
  // there -- where its secondary CPUs sit spinning until the OS releases them
  // through cpu-release-addr / PSCI -- plus their stacks and the DT itself.
  // Nothing else keeps DXE or the OS loader off those pages; bootmgfw
  // reused them and a parked CPU faulted on the overwritten code (run 23).
  //
  if (DeviceTreePtr != NULL) {
    CONST FDT_HEADER  *Hdr = (CONST FDT_HEADER *)DeviceTreePtr;
    CONST UINT8       *Rsv;
    UINTN             Index;

    if (Be32 (&Hdr->Magic) == 0xd00dfeed) {
      Rsv = (CONST UINT8 *)DeviceTreePtr + Be32 (&Hdr->OffMemRsvMap);
      for (Index = 0; Index < 64; Index++, Rsv += 16) {
        UINT64  RsvBase = ((UINT64)Be32 (Rsv) << 32) | Be32 (Rsv + 4);
        UINT64  RsvSize = ((UINT64)Be32 (Rsv + 8) << 32) | Be32 (Rsv + 12);
        if ((RsvBase == 0) && (RsvSize == 0)) {
          break;
        }
        Base = RsvBase & ~(UINT64)(EFI_PAGE_SIZE - 1);
        Size = ((RsvBase - Base) + RsvSize + EFI_PAGE_SIZE - 1) & ~(UINT64)(EFI_PAGE_SIZE - 1);
        DEBUG ((DEBUG_INFO, "M1n1Initrd: FDT memreserve 0x%llx+0x%llx, reserving\n", RsvBase, RsvSize));
        BuildMemoryAllocationHob (Base, Size, EfiReservedMemoryType);
      }
    }
  }

  CONST struct boot_args *Args = (CONST VOID *)(UINTN)FixedPcdGet64 (PcdBootArgsPointer);
  struct memmap Disk;
  size_t Length = 0;
  dt_node_t *Map = dt_get ("/chosen/memory-map");
  VOID *Value = Map == NULL ? NULL : dt_prop (Map, "RAMDisk", &Length);
  if (Value != NULL) {
    if (Length != sizeof (Disk)) return;
    CopyMem (&Disk, Value, sizeof (Disk));
    if (!Disk.size || ((Disk.addr | Disk.size) & EFI_PAGE_MASK) ||
        Disk.addr < Args->phys_base || Disk.size > Args->mem_size ||
        Disk.addr - Args->phys_base > Args->mem_size - Disk.size) return;
    Start = Disk.addr;
    End = Start + Disk.size;
  } else if (!FdtFindInitrd (DeviceTreePtr, &Start, &End)) {
    DEBUG ((DEBUG_INFO, "M1n1Initrd: no initramfs in /chosen, skipping\n"));
    return;
  }

  Base = Start & ~(UINT64)(EFI_PAGE_SIZE - 1);
  Size = ((End - Base) + EFI_PAGE_SIZE - 1) & ~(UINT64)(EFI_PAGE_SIZE - 1);

  DEBUG ((DEBUG_INFO, "M1n1Initrd: payload at 0x%llx-0x%llx (%llu MiB), reserving\n",
          Start, End, (End - Start) >> 20));

  // Keep DXE, the boot manager and the OS loader off this memory until the
  // RAM disk driver takes ownership of it.
  BuildMemoryAllocationHob (Base, Size, EfiReservedMemoryType);

  Hob.Base = Start;
  Hob.Size = End - Start;
  BuildGuidDataHob (&gAppleSiliconPkgM1n1InitrdHobGuid, &Hob, sizeof (Hob));
}

/**
  Keep DXE away from memory that later phases still read through fixed
  addresses: the boot_args + console state + ADT copy EarlySetup made at
  PcdBootArgsPointer, and the iBoot framebuffer (SimpleFbDxe and the
  framebuffer console draw into it). The T602X family reserves these in its
  MemoryInitPeiLib; T600X does not, so do it generically here.

  Must run after MemoryPeim() so the ranges fall inside a system-memory
  resource descriptor.
**/
VOID
EFIAPI
ReserveHandoffRegions (
  VOID
  )
{
  UINT64                  Base;
  UINT64                  Size;
  CONST struct boot_args  *Ba;

  Base = FixedPcdGet64 (PcdBootArgsPointer);
  if (Base != 0) {
    Ba   = (CONST struct boot_args *)(UINTN)Base;
    Size = 0x4000 + Ba->devtree_size;                 /* boot_args @+0, console @+0x2000, ADT @+0x4000 */
    Size = (Size + EFI_PAGE_SIZE - 1) & ~(UINT64)(EFI_PAGE_SIZE - 1);
    BuildMemoryAllocationHob (Base & ~(UINT64)(EFI_PAGE_SIZE - 1), Size, EfiReservedMemoryType);
    DEBUG ((DEBUG_INFO, "ReserveHandoffRegions: boot_args/ADT 0x%llx+0x%llx\n", Base, Size));
    /* 1 MiB plain-text log ring kept by AppleFbUartSerialPortLib at +16 MiB. */
    BuildMemoryAllocationHob ((Base & ~(UINT64)(EFI_PAGE_SIZE - 1)) + 0x1000000, 0x100000, EfiReservedMemoryType);
  }

  Base = PcdGet64 (PcdFrameBufferAddress);
  Size = PcdGet64 (PcdFrameBufferSize);
  if ((Base != 0) && (Size != 0)) {
    Size = ((Base & (EFI_PAGE_SIZE - 1)) + Size + EFI_PAGE_SIZE - 1) & ~(UINT64)(EFI_PAGE_SIZE - 1);
    Base = Base & ~(UINT64)(EFI_PAGE_SIZE - 1);
    BuildMemoryAllocationHob (Base, Size, EfiReservedMemoryType);
    DEBUG ((DEBUG_INFO, "ReserveHandoffRegions: framebuffer 0x%llx+0x%llx\n", Base, Size));
  }
}
