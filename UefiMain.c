#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "PhysicalMemory.h"
#include "VmxHelper.h"
#include "Ept.h"
#include "Vmcs.h"
#include "ComLogger.h"
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Library/DevicePathLib.h>

// --- Local definitions for EFI_MP_SERVICES_PROTOCOL ---
typedef struct _EFI_MP_SERVICES_PROTOCOL EFI_MP_SERVICES_PROTOCOL;

typedef VOID (EFIAPI *EFI_AP_PROCEDURE)(IN VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_MP_SERVICES_GET_NUMBER_OF_PROCESSORS)(
  IN EFI_MP_SERVICES_PROTOCOL *This,
  OUT UINTN *NumberOfProcessors,
  OUT UINTN *NumberOfEnabledProcessors
  );

typedef EFI_STATUS (EFIAPI *EFI_MP_SERVICES_WHO_AM_I)(
  IN EFI_MP_SERVICES_PROTOCOL *This,
  OUT UINTN *ProcessorNumber
  );

typedef EFI_STATUS (EFIAPI *EFI_MP_SERVICES_STARTUP_ALL_APS)(
  IN EFI_MP_SERVICES_PROTOCOL *This,
  IN EFI_AP_PROCEDURE Procedure,
  IN BOOLEAN SingleThread,
  IN EFI_EVENT WaitEvent OPTIONAL,
  IN UINTN TimeoutInMicroSeconds,
  IN VOID *ProcedureArgument OPTIONAL,
  OUT UINTN **FailedCpuList OPTIONAL
  );

typedef EFI_STATUS (EFIAPI *EFI_MP_SERVICES_STARTUP_THIS_AP)(
  IN EFI_MP_SERVICES_PROTOCOL *This,
  IN EFI_AP_PROCEDURE Procedure,
  IN UINTN ProcessorNumber,
  IN EFI_EVENT WaitEvent OPTIONAL,
  IN UINTN TimeoutInMicroSeconds,
  IN VOID *ProcedureArgument OPTIONAL,
  OUT BOOLEAN *Finished OPTIONAL
  );

struct _EFI_MP_SERVICES_PROTOCOL {
  EFI_MP_SERVICES_GET_NUMBER_OF_PROCESSORS GetNumberOfProcessors;
  VOID* GetProcessorInfo;
  EFI_MP_SERVICES_STARTUP_ALL_APS StartupAllAPs;
  EFI_MP_SERVICES_STARTUP_THIS_AP StartupThisAP;
  VOID* SwitchBSP;
  VOID* EnableDisableAP;
  EFI_MP_SERVICES_WHO_AM_I WhoAmI;
};

static EFI_GUID LocalEfiMpServiceProtocolGuid = { 0x3fdda605, 0xa76e, 0x4f46, { 0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08 }};



// Missing EDK II symbols for linker
GLOBAL_REMOVE_IF_UNREFERENCED const UINT32 _gUefiDriverRevision = 0x00010000;
GLOBAL_REMOVE_IF_UNREFERENCED CHAR8 *gEfiCallerBaseName = "SynexUefi";

// Global variables for our hypervisor memory
EFI_PHYSICAL_ADDRESS g_HypervisorMemory = 0;
UINTN g_HypervisorPages = 4096; // 16MB: EPT(514p) + Host PT(514p) + handler + per-core structs

// Original function pointers
typedef
EFI_STATUS
(EFIAPI *EFI_GET_MEMORY_MAP) (
  IN OUT UINTN                  *MemoryMapSize,
  OUT EFI_MEMORY_DESCRIPTOR     *MemoryMap,
  OUT UINTN                     *MapKey,
  OUT UINTN                     *DescriptorSize,
  OUT UINT32                    *DescriptorVersion
  );

typedef
EFI_STATUS
(EFIAPI *EFI_EXIT_BOOT_SERVICES) (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  );

EFI_GET_MEMORY_MAP g_OriginalGetMemoryMap = NULL;
EFI_EXIT_BOOT_SERVICES g_OriginalExitBootServices = NULL;

// Global flag to prevent multiple launches
static BOOLEAN g_HypervisorLaunched = FALSE;

// Helper to print direct Unicode strings to screen when logs are closed
static VOID DirectPrintUefi(IN CHAR16* String)
{
    if (gST && gST->ConOut) {
        gST->ConOut->OutputString(gST->ConOut, String);
    }
}

#define MAX_CORES 128

typedef struct _CORE_RESOURCES {
    UINTN ProcessorIndex;
    EFI_PHYSICAL_ADDRESS VmxonPhys;
    EFI_PHYSICAL_ADDRESS VmcsPhys;
    EFI_PHYSICAL_ADDRESS GdtDest;
    EFI_PHYSICAL_ADDRESS TssPhys;
    EFI_PHYSICAL_ADDRESS HostStackDest;
    EFI_PHYSICAL_ADDRESS Ist1StackDest;
    EFI_PHYSICAL_ADDRESS IdtDest;
    
    volatile EFI_STATUS Status;
    volatile BOOLEAN LaunchCompleted;
} CORE_RESOURCES;

static CORE_RESOURCES g_CoreResources[MAX_CORES];
static EFI_MP_SERVICES_PROTOCOL *g_MpServices = NULL;

VOID
EFIAPI
ApLaunchHypervisor (
  IN VOID *Buffer
  )
{
  UINTN CpuIndex = (UINTN)Buffer;
  if (CpuIndex >= MAX_CORES) {
    return;
  }
  
  CORE_RESOURCES *Res = &g_CoreResources[CpuIndex];
  
  if (!IsVmxSupported()) {
    Res->Status = EFI_UNSUPPORTED;
    Res->LaunchCompleted = TRUE;
    return;
  }
  
  EnableVmxOperation(FALSE);
  
  if (!InitializeVmxon(Res->VmxonPhys, FALSE)) {
    Res->Status = EFI_DEVICE_ERROR;
    Res->LaunchCompleted = TRUE;
    return;
  }
  
  UINT64 HostStackTop = Res->HostStackDest + (8 * 4096);
  UINT64 Ist1StackTop = Res->Ist1StackDest + (2 * 4096);
  
  if (!InitializeVmcsPerCore(
          Res->VmcsPhys,
          Res->GdtDest,
          Res->TssPhys,
          HostStackTop,
          Ist1StackTop,
          Res->IdtDest,
          g_HostCr3,
          g_MsrBitmap,
          g_HandlerDest
          )) {
    Res->Status = EFI_DEVICE_ERROR;
    Res->LaunchCompleted = TRUE;
    return;
  }
  
  Res->Status = EFI_SUCCESS;
  Res->LaunchCompleted = TRUE;
  
  // Launch VMX on this AP core!
  UINT64 StatusLaunch = AsmVmlaunchAndCaptureState();
  if (StatusLaunch == 0) {
    Res->Status = EFI_DEVICE_ERROR;
  }
}

// Our Hooked ExitBootServices
EFI_STATUS
EFIAPI
HookedExitBootServices (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  if (g_HypervisorLaunched) {
      // Second call from Windows after it got EFI_INVALID_PARAMETER on MapKey.
      // Pass straight through to the real firmware ExitBootServices.
      ComPrint("[*] EBS retry: HV already launched. Passing through to original EBS...\r\n");
      EFI_STATUS RetryStatus = g_OriginalExitBootServices(ImageHandle, MapKey);
      if (EFI_ERROR(RetryStatus)) {
          ComPrint("[!] EBS retry FAILED: 0x");
          ComPrintHex((UINT64)RetryStatus);
          ComPrint("\r\n");
      } else {
          ComPrint("[+] EBS retry succeeded! Windows is now taking over.\r\n");
      }
      return RetryStatus;
  }

  // Force reset and enable the text console to draw logs directly to the PC monitor in a premium style!
  if (gST && gST->ConOut) {
      gST->ConOut->Reset(gST->ConOut, FALSE);
      gST->ConOut->SetAttribute(gST->ConOut, 0x0A); // Sleek Matrix Green text on Black background!
      gST->ConOut->ClearScreen(gST->ConOut);
  }

  // We do NOT call LogCloseFile() here anymore to let disk/screen logs capture all initialization steps!
  ComPrint("[SynexHV] ExitBootServices intercepted. Launching hypervisor...\r\n");

  ComPrint("[SynexHV] Checking VMX support...\r\n");
  if (!IsVmxSupported()) {
    ComPrint("[!] VMX not supported on this CPU.\r\n");
    LogCloseFile();
    return g_OriginalExitBootServices(ImageHandle, MapKey);
  }
  ComPrint("[SynexHV] VMX is supported.\r\n");

  // 1. Locate MP Services Protocol
  UINTN NumberOfProcessors = 1;
  UINTN NumberOfEnabledProcessors = 1;
  
  EFI_STATUS MpStatus = gBS->LocateProtocol(&LocalEfiMpServiceProtocolGuid, NULL, (VOID**)&g_MpServices);
  if (EFI_ERROR(MpStatus)) {
    ComPrint("[SynexHV] Warning: EFI_MP_SERVICES_PROTOCOL not found. Operating in single-core fallback.\r\n");
    g_MpServices = NULL;
  } else {
    g_MpServices->GetNumberOfProcessors(g_MpServices, &NumberOfProcessors, &NumberOfEnabledProcessors);
    if (NumberOfProcessors > MAX_CORES) {
      NumberOfProcessors = MAX_CORES;
    }
  }

  ComPrint("[SynexHV] Total cores found: ");
  ComPrintHex((UINT64)NumberOfProcessors);
  ComPrint("\r\n");

  // 2. Preallocate all per-core resources on BSP
  for (UINTN i = 0; i < NumberOfProcessors; i++) {
    g_CoreResources[i].ProcessorIndex = i;
    g_CoreResources[i].VmxonPhys = MemAllocatePages(1);
    g_CoreResources[i].VmcsPhys = MemAllocatePages(1);
    g_CoreResources[i].GdtDest = MemAllocatePages(1);
    g_CoreResources[i].TssPhys = MemAllocatePages(1);
    g_CoreResources[i].HostStackDest = MemAllocatePages(8); // 32KB
    g_CoreResources[i].Ist1StackDest = MemAllocatePages(2); // 8KB
    g_CoreResources[i].IdtDest = MemAllocatePages(1);
    
    g_CoreResources[i].Status = EFI_NOT_STARTED;
    g_CoreResources[i].LaunchCompleted = FALSE;
    
    if (!g_CoreResources[i].VmxonPhys || !g_CoreResources[i].VmcsPhys ||
        !g_CoreResources[i].GdtDest || !g_CoreResources[i].TssPhys ||
        !g_CoreResources[i].HostStackDest || !g_CoreResources[i].Ist1StackDest ||
        !g_CoreResources[i].IdtDest) {
      ComPrint("[!] Failed to allocate resources for core ");
      ComPrintHex((UINT64)i);
      ComPrint("\r\n");
      LogCloseFile();
      return g_OriginalExitBootServices(ImageHandle, MapKey);
    }
  }
  ComPrint("[+] Resources allocated for all cores.\r\n");

  // 3. Prepare Shared Resources (MSR Bitmap, VM-Exit handler stub, Host CR3)
  if (!PrepareSharedResources()) {
    ComPrint("[!] Failed to prepare shared hypervisor resources.\r\n");
    LogCloseFile();
    return g_OriginalExitBootServices(ImageHandle, MapKey);
  }
  ComPrint("[+] Shared hypervisor resources prepared.\r\n");

  // 4. Build EPT identity map (shared across all cores)
  ComPrint("[SynexHV] Building EPT identity map...\r\n");
  EPTP Eptp = InitializeEpt();
  if (Eptp.All == 0) {
    ComPrint("[!] EPT init failed.\r\n");
    LogCloseFile();
    return g_OriginalExitBootServices(ImageHandle, MapKey);
  }
  ComPrint("[+] EPT 1:1 map built.\r\n");

  // 5. Start all Application Processors (APs) sequentially
  if (g_MpServices != NULL && NumberOfProcessors > 1) {
    ComPrint("[SynexHV] Launching hypervisor on APs sequentially...\r\n");
    for (UINTN i = 1; i < NumberOfProcessors; i++) {
      ComPrint("[SynexHV] Starting AP Core 0x");
      ComPrintHex((UINT64)i);
      ComPrint("...\r\n");
      
      MpStatus = g_MpServices->StartupThisAP(
                                 g_MpServices,
                                 ApLaunchHypervisor,
                                 i,                  // ProcessorNumber
                                 NULL,               // WaitEvent (NULL = blocking wait)
                                 0,                  // TimeoutInMicroSeconds (0 = wait forever)
                                 (VOID*)i,           // ProcedureArgument (CpuIndex passed directly)
                                 NULL                // Finished
                                 );
      if (EFI_ERROR(MpStatus)) {
        ComPrint("[!] StartupThisAP failed for core 0x");
        ComPrintHex((UINT64)i);
        ComPrint(" Status: 0x");
        ComPrintHex((UINT64)MpStatus);
        ComPrint("\r\n");
      } else {
        ComPrint("[+] Core 0x");
        ComPrintHex((UINT64)i);
        ComPrint(" virtualized successfully.\r\n");
      }
    }
  }

  // 6. Virtualize Bootstrap Processor (BSP / Core 0)
  ComPrint("[SynexHV] Virtualizing Bootstrap Processor (BSP / Core 0)...\r\n");
  CORE_RESOURCES *BspRes = &g_CoreResources[0];
  
  BspRes->Status = EFI_SUCCESS;
  BspRes->LaunchCompleted = TRUE;
  
  EnableVmxOperation(TRUE);
  if (!InitializeVmxon(BspRes->VmxonPhys, TRUE)) {
    ComPrint("[!] BSP VMXON failed.\r\n");
    LogCloseFile();
    return g_OriginalExitBootServices(ImageHandle, MapKey);
  }
  
  UINT64 BspHostStackTop = BspRes->HostStackDest + (8 * 4096);
  UINT64 BspIst1StackTop = BspRes->Ist1StackDest + (2 * 4096);
  
  if (!InitializeVmcsPerCore(
          BspRes->VmcsPhys,
          BspRes->GdtDest,
          BspRes->TssPhys,
          BspHostStackTop,
          BspIst1StackTop,
          BspRes->IdtDest,
          g_HostCr3,
          g_MsrBitmap,
          g_HandlerDest
          )) {
    ComPrint("[!] BSP VMCS configuration failed.\r\n");
    LogCloseFile();
    return g_OriginalExitBootServices(ImageHandle, MapKey);
  }
  
  ComPrint("[+] BSP VMCS configured. Executing VMLAUNCH on BSP...\r\n");
  g_HypervisorLaunched = TRUE;

  // Let disk/screen logs finish writing before starting the VMX non-root state
  LogCloseFile();

  UINT64 StatusLaunch = AsmVmlaunchAndCaptureState();
  if (StatusLaunch == 0) {
      UINTN VmError = 0;
      __vmx_vmread(0x4400, &VmError);
      ComPrint("[!] BSP VMLAUNCH failed! VM_INSTRUCTION_ERROR = 0x");
      ComPrintHex(VmError);
      ComPrint("\r\n");
  } else {
      // Guest context — BSP VMLAUNCH succeeded.
      ComPrint("[+] BSP VMLAUNCH succeeded! Guest is now running in VMX Non-Root mode.\r\n");
  }

  // Call the original ExitBootServices.
  ComPrint("[*] Calling original ExitBootServices (MapKey=0x");
  ComPrintHex((UINT64)MapKey);
  ComPrint(")...\r\n");

  LogDisableOutput();
  EFI_STATUS EbsStatus = g_OriginalExitBootServices(ImageHandle, MapKey);

  // If we get here, ExitBootServices returned an error.
  ComPrint("[!] ExitBootServices returned error 0x");
  ComPrintHex((UINT64)EbsStatus);
  ComPrint(" — Windows will retry.\r\n");
  return EbsStatus;
}

// Our Hooked GetMemoryMap
EFI_STATUS
EFIAPI
HookedGetMemoryMap (
  IN OUT UINTN                  *MemoryMapSize,
  OUT EFI_MEMORY_DESCRIPTOR     *MemoryMap,
  OUT UINTN                     *MapKey,
  OUT UINTN                     *DescriptorSize,
  OUT UINT32                    *DescriptorVersion
  )
{
  EFI_STATUS Status;
  
  // Call original
  Status = g_OriginalGetMemoryMap(MemoryMapSize, MemoryMap, MapKey, DescriptorSize, DescriptorVersion);
  
  // If the call succeeds and we have a map to modify
  if (!EFI_ERROR(Status) && MemoryMap != NULL) {
    UINTN NumEntries = *MemoryMapSize / *DescriptorSize;
    EFI_MEMORY_DESCRIPTOR *Desc = MemoryMap;
    
    for (UINTN i = 0; i < NumEntries; i++) {
      // Check if this memory descriptor contains our allocated hypervisor memory
      if (g_HypervisorMemory >= Desc->PhysicalStart && 
          g_HypervisorMemory < (Desc->PhysicalStart + (Desc->NumberOfPages * EFI_PAGE_SIZE))) {
        
        // Hide it! Mark it as Reserved or RuntimeServicesData so Windows OS ignores it.
        // We use EfiReservedMemoryType so it is completely excluded from the Windows PFN database.
        Desc->Type = EfiReservedMemoryType;
      }
      Desc = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)Desc + *DescriptorSize);
    }
  }
  
  return Status;
}

EFI_STATUS
EFIAPI
UefiUnload (
    IN EFI_HANDLE ImageHandle
    )
{
    return EFI_SUCCESS;
}

// Function to find and start the real Windows Boot Manager
EFI_STATUS
ChainloadWindows (
  IN EFI_HANDLE ImageHandle
  )
{
  EFI_STATUS Status;
  UINTN HandleCount;
  EFI_HANDLE *Handles;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  EFI_HANDLE WinBootManagerHandle = NULL;

  // 1. Locate all file systems
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR(Status)) return Status;

  for (UINTN i = 0; i < HandleCount; i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
    Status = gBS->HandleProtocol(Handles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Fs);
    if (EFI_ERROR(Status)) continue;

    EFI_FILE_PROTOCOL *Root;
    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status)) continue;

    // Try to open the original Windows Boot Manager
    EFI_FILE_PROTOCOL *File;
    Status = Root->Open(Root, &File, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(Status)) {
      File->Close(File);
      WinBootManagerHandle = Handles[i];
      ComPrint("[+] Found Windows Boot Manager on a partition.\r\n");
      
      // Create full device path for the file
      DevicePath = FileDevicePath(WinBootManagerHandle, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
      
      // Load the image into memory
      EFI_HANDLE NewImageHandle;
      Status = gBS->LoadImage(FALSE, ImageHandle, DevicePath, NULL, 0, &NewImageHandle);
      if (!EFI_ERROR(Status)) {
        ComPrint("[*] Starting Windows Boot Manager...\r\n");
        // Hand off control to Windows. 
        // Our ExitBootServices hook will trigger later when Windows starts kernel loading.
        return gBS->StartImage(NewImageHandle, NULL, NULL);
      }
    }
    Root->Close(Root);
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  // Clear the screen for a clean output
  gST->ConOut->ClearScreen(gST->ConOut);
  Print(L"\n====================================\n");
  Print(L"[SynexUefi] Hypervisor Bootkit Loading...\n");
  
  // Initialize COM Port Logger
  ComInit();
  ComPrint("[SynexUefi] Bootkit started. COM Logger initialized.\r\n");
  
  // 1. Allocate 8MB of memory for the Hypervisor Heap/VMCS/EPT
  g_HypervisorMemory = 0xFFFFFFFF;
  Status = gBS->AllocatePages(AllocateMaxAddress, EfiRuntimeServicesData, g_HypervisorPages, &g_HypervisorMemory);
  
  if (EFI_ERROR(Status)) {
    Print(L"[!] Failed to allocate Hypervisor memory! Status: %r\n", Status);
    gBS->Stall(5000000);
    return Status;
  }
  
  Print(L"[+] Allocated 8MB of Hypervisor memory at Physical Address: 0x%llx\n", g_HypervisorMemory);

  // Initialize our custom bump allocator
  MemInit(g_HypervisorMemory, g_HypervisorPages);

  // Initialize advanced logging (synex_boot.log on the boot partition)
  LogInitFile(ImageHandle);

  // Allocate VMX-Root Circular Log Buffer
  LogInitBuffer();

  ComPrint("[SynexUefi] Log file and memory log buffer initialized successfully.\r\n");


  // NOTE: GetMemoryMap hook is NOT installed.
  // We allocated hypervisor memory as EfiRuntimeServicesData which Windows
  // naturally preserves without needing to hide it.  Hooking GetMemoryMap
  // corrupts MapKey integrity and causes STATUS_UNSUCCESSFUL (0xc0000001)
  // because the Boot Manager calls GetMemoryMap multiple times to validate
  // the key before calling ExitBootServices.

  // Hook ExitBootServices only
  g_OriginalExitBootServices = gBS->ExitBootServices;
  gBS->ExitBootServices = HookedExitBootServices;

  // Update the CRC32 of the Boot Services Table
  gBS->Hdr.CRC32 = 0;
  gBS->CalculateCrc32(gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);

  Print(L"[+] Hypervisor memory type: EfiRuntimeServicesData (preserved by Windows).\n");
  Print(L"[+] ExitBootServices Hooked! Ready to catch OS handoff.\n");

  // 4. Hand off to the OS Boot Manager
  Print(L"[*] Searching for Windows Boot Manager...\n");
  Status = ChainloadWindows(ImageHandle);

  if (EFI_ERROR(Status)) {
      Print(L"[!] Failed to find/start Windows Boot Manager! Status: %r\n", Status);
      Print(L"[*] Please ensure Windows is installed on an EFI partition.\n");
      gBS->Stall(10000000);
  }
  
  return Status;
}
