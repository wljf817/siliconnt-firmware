#MacBook Pro 2021 UEFI DSC file
#some parts borrowed from WOA-Project/SurfaceDuoPkg
#Disclaimer: probably not the best UEFI dev out there
#
#  Copyright (c) 2011-2015, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2016, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#
#  This program and the accompanying materials
#  are licensed and made available under the terms and conditions of the BSD License
#  which accompanies this distribution.  The full text of the license may be found at
#  http://opensource.org/licenses/bsd-license.php
#
#  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
#  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
#
#

#SPDX-License-Identifier: BSD 2-Clause

#Basic Defines

[Defines]
  PLATFORM_NAME                  = MacBookPro2021
  PLATFORM_GUID                  = 6951be29-0709-433a-81db-046d9210cd38
  PLATFORM_VERSION               = 1.0
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/MacBookPro2021-$(ARCH)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = MacBookPro2021Pkg/MacBookPro2021.fdf
  SECURE_BOOT_ENABLE             = FALSE #disable secure boot for now
  # FALSE: vanilla ArmGicDxe against the GICv3 that m1n1's hypervisor emulates at
  # PcdGicDistributorBase/RedistributorsBase (boot.bin carries hv=1, see README).
  # TRUE (the AIC/no-hypervisor route) got as far as bootmgfw.efi, which then
  # drops itself to EL1 and needs a hypervisor underneath anyway.
  AIC_BUILD                      = FALSE
  NETWORK_TLS_ENABLE             = TRUE
  #
  # J314/J316 ship with 16 or 32 GB. T600XFamilyPkg's default boot_args/ADT
  # copy destination (+40 GB) is outside RAM on these machines and hangs PrePi
  # before any output; use +8.5 GB like the T602X laptops.
  #
  T600X_BOOTARGS_POINTER         = 0x10220000000
  T600X_ADT_POINTER              = 0x10220004000
  #
  # No serial adapter on a laptop: draw the DEBUG console into the iBoot
  # framebuffer as well (AppleFbUartSerialPortLib, patches/0002).
  #
  APPLE_SERIAL_PORT_LIB          = AppleSiliconPkg/Library/AppleFbUartSerialPortLib/AppleFbUartSerialPortLib.inf
  # m1n1 leaves us at EL2 with no PSCI provider: reboot through the watchdog.
  APPLE_HW_RESET_SYSTEM_LIB      = AppleSiliconPkg/Library/AppleWdtResetSystemLib/AppleWdtResetSystemLib.inf


[BuildOptions.common]
  GCC:*_*_AARCH64_CC_FLAGS = -DSILICON_PLATFORM=6000
  #*_*_*_CC_FLAGS = -D DISABLE_NEW_DEPRECATED_INTERFACES -D HAS_MEMCPY_INTRINSICS


[PcdsFixedAtBuild.common]
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemModel|"MacBook Pro (14-inch/16-inch, 2021)"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemModelNumber|"MacBookPro18,1/18,2/18,3/18,4"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemSku|"MacBook Pro (MacBookPro18,x)"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosSystemFamily|"MacBook Pro"
  # Panel defaults (14" J314: 3024x1964; 16" J316: 3456x2234). SimpleFbDxe takes the
  # real geometry from boot_args, these only seed the setup-video PCDs.
  gAppleSiliconPkgTokenSpaceGuid.PcdFrameBufferWidth|3024
  gAppleSiliconPkgTokenSpaceGuid.PcdFrameBufferHeight|1964
  gAppleSiliconPkgTokenSpaceGuid.PcdFrameBufferPixelBpp|30
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleNumDwc3Controllers|3 # J314/J316: 3 USB-C ports (dwc3_0..2); atcphy3 is used for HDMI
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleNumDwc3Darts|6 # 2 DARTs per dwc3 controller
  #will be changed later on, default values
  # gAppleSiliconPkgTokenSpaceGuid.PcdFrameBufferWidth|1920
  # gAppleSiliconPkgTokenSpaceGuid.PcdFrameBufferHeight|1080
  # gAppleSiliconPkgTokenSpaceGuid.PcdFrameBufferPixelBpp|30
  
[PcdsDynamicDefault.common]
  # #borrowed from SurfaceDuoPkg
  # gEfiMdeModulePkgTokenSpaceGuid.PcdVideoHorizontalResolution|1920
  # gEfiMdeModulePkgTokenSpaceGuid.PcdVideoVerticalResolution|1080
  # gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|1920
  # gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|1080
  # gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|300
  # gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|50
  # gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|300 
  # gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|50

[Components.common]

  MacBookPro2021Pkg/AcpiTables/DeviceAcpiTables.inf
  # AcpiPlatformDxe ASSERTs unless all four table storage files exist in the FV:
  # device (above), device family (laptop FADT), SoC (T600X CSRT) and generic.
  MacBookProFamilyPkg/AcpiTables/DeviceFamilyAcpiTables.inf
  # The Mu front page / boot menu draw through the Simple Window Manager, which
  # only comes up once a Rendering Engine sits on top of GOP. FrontpageDsc.inc
  # leaves both out; we need the boot menu, so add them here.
  MsGraphicsPkg/GopOverrideDxe/GopOverrideDxe.inf
  MsGraphicsPkg/RenderingEngineDxe/RenderingEngineDxe.inf
  #
  # PCIe support is disabled for now.
  #
  #AppleSiliconPkg/Drivers/AppleBootTimeEmbeddedFirmwareHelperDxe/AppleBootTimeEmbeddedFirmwareHelperDxe.inf


!include T600XFamilyPkg/T600XFamilyPkg.dsc.inc
!include AppleSiliconPkg/AppleSiliconPkg.dsc.inc
!include AppleSiliconPkg/FrontpageDsc.inc
# Overrides of values set by the includes above (last definition wins).
[LibraryClasses.common]
  # Exception dump that prints ESR/FAR/registers before walking a stack it
  # cannot trust (needed to see why a foreign image like bootmgfw faults).
  DefaultExceptionHandlerLib|AppleSiliconPkg/Library/AppleDefaultExceptionHandlerLib/AppleDefaultExceptionHandlerLib.inf
  # Exception vectors that stay on the current stack for same-EL synchronous
  # exceptions: bootmgfw leaves SP_EL0 truncated to 32 bits, which made the
  # upstream SP_EL0 switch fault before anything useful was printed.
  CpuExceptionHandlerLib|AppleSiliconPkg/Library/AppleArmExceptionLib/AppleArmExceptionLib.inf

[PcdsFixedAtBuild.common]
  # The framebuffer console is the only log channel we have on a laptop, and one
  # screenful is all that survives a hang. Drop the firehose bits (POOL/PAGE/
  # GCD/EVENT/CACHE/VERBOSE/...) so what stays on screen is ERROR, INIT, WARN,
  # LOAD, FS, INFO, VARIABLE and BM (DISPATCH too: its DEPEX dumps drowned run 4).
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8000054F
  # Nothing bootable -> replay the RAM disk / boot option log lines on the panel
  # and hold them 25 s before the front page (bring-up aid, 0 to disable).
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleBootFailureLogSeconds|25
