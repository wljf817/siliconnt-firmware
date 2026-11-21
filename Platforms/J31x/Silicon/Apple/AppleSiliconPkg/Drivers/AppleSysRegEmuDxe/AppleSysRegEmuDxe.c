/** @file
  Apple cores implement no ARM PMU (ID_AA64DFR0_EL1.PMUVer = 0), so every
  PMCCNTR_EL0 / PMCR_EL0 / ... access is UNDEFINED. Windows Boot Manager reads
  PMCCNTR_EL0 during its early init (seen at bootmgfw+0x3EDD4 on the J314)
  and takes a Synchronous Exception with EC 0 on us.

  Under m1n1's hypervisor (NTASP's vGIC route) those accesses trap to EL2 and
  are emulated there. On this no-hypervisor route the firmware owns the EL2
  vector table until the OS installs its own, so emulate here: hook the
  synchronous exception, decode MRS/MSR at ELR, satisfy PMU register reads
  with harmless values (the cycle counter maps to the 24 MHz generic timer),
  swallow writes, skip the instruction. Anything else goes to the normal
  crash dump.

  Only covers the time the firmware's vectors are live (bootmgfw, winload).
  The NT kernel will need the same thing done by a hypervisor.

  SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
**/
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/ArmLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DefaultExceptionHandlerLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/DebugSupport.h>

#define ESR_EC(Esr)  (((Esr) >> 26) & 0x3F)
#define EC_UNKNOWN   0x00

/* MRS/MSR (register) encoding: 1101 0101 00 L 1 o0 op1 CRn CRm op2 Rt */
#define SYSREG_MASK        0xFFD00000u
#define SYSREG_MATCH       0xD5100000u
#define SYSREG_IS_READ(I)  (((I) & BIT21) != 0)
#define SYSREG_OP0(I)      (2 + (((I) >> 19) & 1))
#define SYSREG_OP1(I)      (((I) >> 16) & 7)
#define SYSREG_CRN(I)      (((I) >> 12) & 0xF)
#define SYSREG_CRM(I)      (((I) >> 8) & 0xF)
#define SYSREG_OP2(I)      (((I) >> 5) & 7)
#define SYSREG_RT(I)       ((I) & 0x1F)

STATIC UINTN  mEmulated;

/**
  Is this a register of the ARMv8 PMU (the ones an OS pokes when it thinks a
  PMU exists)? op0=3: op1=3 CRn=9 CRm=12..14 (PMCR, PMCNTEN*, PMOVSCLR,
  PMSWINC, PMSELR, PMCEID*, PMCCNTR, PMXEV*, PMUSERENR, PMOVSSET),
  op1=3 CRn=14 CRm=8..15 (PMEVCNTRn, PMEVTYPERn, PMCCFILTR), and op1=0 CRn=9
  CRm=14 (PMINTENSET/CLR_EL1).
**/
STATIC
BOOLEAN
IsPmuRegister (
  IN UINT32  Insn
  )
{
  UINTN  Op1 = SYSREG_OP1 (Insn);
  UINTN  CRn = SYSREG_CRN (Insn);
  UINTN  CRm = SYSREG_CRM (Insn);

  if (SYSREG_OP0 (Insn) != 3) {
    return FALSE;
  }
  if ((Op1 == 3) && (CRn == 9) && (CRm >= 12) && (CRm <= 14)) {
    return TRUE;
  }
  if ((Op1 == 3) && (CRn == 14) && (CRm >= 8)) {
    return TRUE;
  }
  if ((Op1 == 0) && (CRn == 9) && (CRm == 14)) {
    return TRUE;
  }
  return FALSE;
}

STATIC
UINT64
PmuReadValue (
  IN UINT32  Insn
  )
{
  /* PMCCNTR_EL0 (3,3,9,13,0): a monotonically increasing count is all
     callers want; the generic timer is the closest honest thing we have. */
  if ((SYSREG_OP1 (Insn) == 3) && (SYSREG_CRN (Insn) == 9) &&
      (SYSREG_CRM (Insn) == 13) && (SYSREG_OP2 (Insn) == 0)) {
    return ArmReadCntPct ();
  }
  /* PMCR_EL0 and friends: N=0 counters, nothing enabled, nothing implemented. */
  return 0;
}

STATIC
VOID
EFIAPI
SyncExceptionHandler (
  IN     EFI_EXCEPTION_TYPE  ExceptionType,
  IN OUT EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  EFI_SYSTEM_CONTEXT_AARCH64  *Ctx = SystemContext.SystemContextAArch64;
  UINT32                      Insn;
  UINT64                      *Regs;
  UINTN                       Rt;

  if (ESR_EC (Ctx->ESR) == EC_UNKNOWN) {
    Insn = *(UINT32 *)(UINTN)Ctx->ELR;
    if (((Insn & SYSREG_MASK) == SYSREG_MATCH) && IsPmuRegister (Insn)) {
      Regs = &Ctx->X0;                 /* X0..X28, FP, LR are consecutive */
      Rt   = SYSREG_RT (Insn);
      if (SYSREG_IS_READ (Insn)) {
        if (Rt != 31) {
          Regs[Rt] = PmuReadValue (Insn);
        }
      }
      /* writes: ignored */
      Ctx->ELR += 4;
      if (mEmulated++ < 8) {
        DEBUG ((DEBUG_INFO, "SysRegEmu: %a S3_%u_C%u_C%u_%u at 0x%lx (insn %08x) -> %a\n",
                SYSREG_IS_READ (Insn) ? "MRS" : "MSR", SYSREG_OP1 (Insn), SYSREG_CRN (Insn),
                SYSREG_CRM (Insn), SYSREG_OP2 (Insn), Ctx->ELR - 4, Insn,
                SYSREG_IS_READ (Insn) ? "emulated" : "ignored"));
      }
      return;
    }
  }

  DefaultExceptionHandler (ExceptionType, SystemContext);
}

EFI_STATUS
EFIAPI
AppleSysRegEmuDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_CPU_ARCH_PROTOCOL  *Cpu;

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, SyncExceptionHandler);
  DEBUG ((DEBUG_INFO, "AppleSysRegEmuDxe: PMU register emulation %a (%r)\n",
          EFI_ERROR (Status) ? "NOT installed" : "installed", Status));
  return Status;
}
