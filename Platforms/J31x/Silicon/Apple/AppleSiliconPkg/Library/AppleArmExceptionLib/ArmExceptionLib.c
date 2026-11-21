/* @file
*  Main file supporting the SEC Phase for Versatile Express
*
*  Copyright (c) 2008 - 2009, Apple Inc. All rights reserved.<BR>
*  Copyright (c) 2011-2021, Arm Limited. All rights reserved.<BR>
*  Copyright (c) 2016 HP Development Company, L.P.
*  Copyright (c) 2022, Intel Corporation. All rights reserved.<BR>
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#include <Uefi.h>
#include <Library/CpuExceptionHandlerLib.h>

#include <Library/ArmLib.h>
#include <Library/PcdLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DefaultExceptionHandlerLib.h>

CHAR8 *
GetImageName (
  IN  UINTN  FaultAddress,
  OUT UINTN  *ImageBase,
  OUT UINTN  *PeCoffSizeOfHeaders
  );

STATIC
CONST CHAR8 *
BaseName (
  IN CONST CHAR8  *FullName
  )
{
  CONST CHAR8  *Str = FullName + AsciiStrLen (FullName);
  while (--Str > FullName) {
    if ((*Str == '/') || (*Str == '\\')) {
      return Str + 1;
    }
  }
  return Str;
}

VOID
ExceptionHandlersStart (
  VOID
  );

VOID
ExceptionHandlersEnd (
  VOID
  );

RETURN_STATUS
ArchVectorConfig (
  IN  UINTN  VectorBaseAddress
  );

// these globals are provided by the architecture specific source (Arm or AArch64)
extern UINTN                   gMaxExceptionNumber;
extern EFI_EXCEPTION_CALLBACK  gExceptionHandlers[];
extern PHYSICAL_ADDRESS        gExceptionVectorAlignmentMask;

/**
Initializes all CPU exceptions entries and provides the default exception handlers.

Caller should try to get an array of interrupt and/or exception vectors that are in use and need to
persist by EFI_VECTOR_HANDOFF_INFO defined in PI 1.3 specification.
If caller cannot get reserved vector list or it does not exists, set VectorInfo to NULL.
If VectorInfo is not NULL, the exception vectors will be initialized per vector attribute accordingly.

@param[in]  VectorInfo    Pointer to reserved vector list.

@retval EFI_SUCCESS           CPU Exception Entries have been successfully initialized
with default exception handlers.
@retval EFI_INVALID_PARAMETER VectorInfo includes the invalid content if VectorInfo is not NULL.
@retval EFI_UNSUPPORTED       This function is not supported.

**/
EFI_STATUS
EFIAPI
InitializeCpuExceptionHandlers (
  IN EFI_VECTOR_HANDOFF_INFO  *VectorInfo OPTIONAL
  )
{
  UINT64  VectorBase;

  // use VBAR to point to where our exception handlers are

  // The vector table must be aligned for the architecture.  If this
  // assertion fails ensure the appropriate FFS alignment is in effect,
  // which can be accomplished by ensuring the proper Align=X statement
  // in the platform packaging rules.  For ARM Align=32 is required and
  // for AArch64 Align=4K is required.  Align=Auto can be used but this
  // is known to cause an issue with populating the reset vector area
  // for encapsulated FVs.
  ASSERT (((UINTN)ExceptionHandlersStart & gExceptionVectorAlignmentMask) == 0);

  VectorBase = (UINT64)(UINTN)ExceptionHandlersStart;

  // call the architecture-specific routine to prepare for the new vector
  // configuration to take effect
  ArchVectorConfig ((UINTN)VectorBase);

  ArmWriteVBar ((UINTN)VectorBase);

  return RETURN_SUCCESS;
}

/**
Registers a function to be called from the processor exception handler. (On ARM/AArch64 this only
provides exception handlers, not interrupt handling which is provided through the Hardware Interrupt
Protocol.)

This function registers and enables the handler specified by ExceptionHandler for a processor
interrupt or exception type specified by ExceptionType. If ExceptionHandler is NULL, then the
handler for the processor interrupt or exception type specified by ExceptionType is uninstalled.
The installed handler is called once for each processor interrupt or exception.
NOTE: This function should be invoked after InitializeCpuExceptionHandlers() is invoked,
otherwise EFI_UNSUPPORTED returned.

@param[in]  ExceptionType     Defines which interrupt or exception to hook.
@param[in]  ExceptionHandler  A pointer to a function of type EFI_CPU_INTERRUPT_HANDLER that is called
when a processor interrupt occurs. If this parameter is NULL, then the handler
will be uninstalled.

@retval EFI_SUCCESS           The handler for the processor interrupt was successfully installed or uninstalled.
@retval EFI_ALREADY_STARTED   ExceptionHandler is not NULL, and a handler for ExceptionType was
previously installed.
@retval EFI_INVALID_PARAMETER ExceptionHandler is NULL, and a handler for ExceptionType was not
previously installed.
@retval EFI_UNSUPPORTED       The interrupt specified by ExceptionType is not supported,
or this function is not supported.
**/
RETURN_STATUS
RegisterCpuInterruptHandler (
  IN EFI_EXCEPTION_TYPE         ExceptionType,
  IN EFI_CPU_INTERRUPT_HANDLER  ExceptionHandler
  )
{
  if ((UINTN)ExceptionType > gMaxExceptionNumber) {
    return RETURN_UNSUPPORTED;
  }

  if ((ExceptionHandler != NULL) && (gExceptionHandlers[ExceptionType] != NULL)) {
    return RETURN_ALREADY_STARTED;
  }

  gExceptionHandlers[ExceptionType] = ExceptionHandler;

  return RETURN_SUCCESS;
}

VOID
EFIAPI
CommonCExceptionHandler (
  IN     EFI_EXCEPTION_TYPE  ExceptionType,
  IN OUT EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  //
  // Bring-up sampler: every 128 timer ticks (~1.3 s at the 10 ms Mu period) print
  // a mini backtrace of where the CPU was when the IRQ arrived, so a silent
  // foreign image (bootmgfw) shows what it is spinning or waiting on even when
  // the interrupted PC itself is inside a firmware service. Repeats of the same
  // interrupted PC are printed once. No lines at all = it runs with IRQs masked.
  //
  if (ExceptionType == EXCEPT_AARCH64_IRQ) {
    STATIC UINTN  IrqCount = 0;
    STATIC UINTN  LastElr  = 0;
    if ((++IrqCount & 0x7F) == 0) {
      EFI_SYSTEM_CONTEXT_AARCH64  *Ctx = SystemContext.SystemContextAArch64;
      if (Ctx->ELR != LastElr) {
        UINTN   Base, Hdr, Depth;
        UINT64  *Fp;
        CHAR8   *Pdb;
        LastElr = Ctx->ELR;
        Pdb = GetImageName (Ctx->ELR, &Base, &Hdr);
        DEBUG ((DEBUG_ERROR, "IRQ sample: %a+0x%lx", (Pdb != NULL) ? BaseName (Pdb) : "?", (Pdb != NULL) ? Ctx->ELR - Base : Ctx->ELR));
        Pdb = GetImageName (Ctx->LR, &Base, &Hdr);
        DEBUG ((DEBUG_ERROR, " lr %a+0x%lx", (Pdb != NULL) ? BaseName (Pdb) : "?", (Pdb != NULL) ? Ctx->LR - Base : Ctx->LR));
        Fp = (UINT64 *)Ctx->FP;
        for (Depth = 0; Depth < 12; Depth++) {
          UINT64  Addr = (UINT64)(UINTN)Fp;
          if ((Addr == 0) || ((Addr & 0xF) != 0) || (Addr < Ctx->SP) || (Addr - Ctx->SP > SIZE_4MB)) {
            break;
          }
          Pdb = GetImageName (Fp[1], &Base, &Hdr);
          DEBUG ((DEBUG_ERROR, " < %a+0x%lx", (Pdb != NULL) ? BaseName (Pdb) : "?", (Pdb != NULL) ? Fp[1] - Base : Fp[1]));
          Fp = (UINT64 *)Fp[0];
        }
        DEBUG ((DEBUG_ERROR, "\n"));
      }
    }
  }

  if ((UINTN)ExceptionType <= gMaxExceptionNumber) {
    if (gExceptionHandlers[ExceptionType]) {
      gExceptionHandlers[ExceptionType](ExceptionType, SystemContext);
      return;
    }
  } else {
    DEBUG ((DEBUG_ERROR, "Unknown exception type %d\n", ExceptionType));
    ASSERT (FALSE);
  }

  DefaultExceptionHandler (ExceptionType, SystemContext);
}

/**
  Setup separate stacks for certain exception handlers.
  If the input Buffer and BufferSize are both NULL, use global variable if possible.

  @param[in]       Buffer        Point to buffer used to separate exception stack.
  @param[in, out]  BufferSize    On input, it indicates the byte size of Buffer.
                                 If the size is not enough, the return status will
                                 be EFI_BUFFER_TOO_SMALL, and output BufferSize
                                 will be the size it needs.

  @retval EFI_SUCCESS             The stacks are assigned successfully.
  @retval EFI_UNSUPPORTED         This function is not supported.
  @retval EFI_BUFFER_TOO_SMALL    This BufferSize is too small.
**/
EFI_STATUS
EFIAPI
InitializeSeparateExceptionStacks (
  IN     VOID   *Buffer,
  IN OUT UINTN  *BufferSize
  )
{
  return EFI_SUCCESS;
}
