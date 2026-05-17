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

// Missing EDK II symbols for linker
GLOBAL_REMOVE_IF_UNREFERENCED const UINT32 _gUefiDriverRevision = 0x00010000;
GLOBAL_REMOVE_IF_UNREFERENCED CHAR8 *gEfiCallerBaseName = "SynexUefi";

// Global variables for our hypervisor memory
EFI_PHYSICAL_ADDRESS g_HypervisorMemory = 0;
UINTN g_HypervisorPages = 2048; // 8MB (2048 * 4KB pages) to hold 512GB EPT Identity Map

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

  // We do NOT call LogCloseFile() here anymore to let disk/screen logs capture all initialization steps!
  ComPrint("[SynexHV] ExitBootServices intercepted. Launching hypervisor...\r\n");

  ComPrint("[SynexHV] Checking VMX support...\r\n");
  if (!IsVmxSupported()) {
    ComPrint("[!] VMX not supported on this CPU.\r\n");
    LogCloseFile();
    return g_OriginalExitBootServices(ImageHandle, MapKey);
  }
  ComPrint("[SynexHV] VMX is supported.\r\n");

  ComPrint("[SynexHV] Enabling VMX in CR4...\r\n");
  EnableVmxOperation();
  ComPrint("[SynexHV] VMX enabled in CR4.\r\n");

  ComPrint("[SynexHV] Entering VMX root mode (VMXON)...\r\n");
  if (!InitializeVmxon()) {
    ComPrint("[!] VMXON failed.\r\n");
    LogCloseFile();
    return g_OriginalExitBootServices(ImageHandle, MapKey);
  }
  ComPrint("[+] VMXON success. In VMX Root Mode.\r\n");

  ComPrint("[SynexHV] Building EPT identity map...\r\n");
  EPTP Eptp = InitializeEpt();
  if (Eptp.All == 0) {
    ComPrint("[!] EPT init failed.\r\n");
    LogCloseFile();
    return g_OriginalExitBootServices(ImageHandle, MapKey);
  }
  ComPrint("[+] EPT 1:1 map built.\r\n");

  ComPrint("[SynexHV] Configuring VMCS fields...\r\n");
  if (!InitializeVmcs()) {
    ComPrint("[!] VMCS setup failed.\r\n");
    LogCloseFile();
    return g_OriginalExitBootServices(ImageHandle, MapKey);
  }
  ComPrint("[+] VMCS configured. Ready for VMLAUNCH.\r\n");

  g_HypervisorLaunched = TRUE;

  ComPrint("[SynexHV] Executing AsmVmlaunchAndCaptureState (VMLAUNCH) now...\r\n");

  UINT64 StatusLaunch = AsmVmlaunchAndCaptureState();
  if (StatusLaunch == 0) {
      UINTN VmError = 0;
      __vmx_vmread(0x4400, &VmError);
      ComPrint("[!] VMLAUNCH failed! VM_INSTRUCTION_ERROR = 0x");
      ComPrintHex(VmError);
      ComPrint("\r\n");
      LogCloseFile();
  } else {
      // Guest context — VMLAUNCH succeeded.
      ComPrint("[+] VMLAUNCH succeeded! Guest is now running in VMX Non-Root mode.\r\n");
  }

  // Both the host-failure path and the guest-success path reach here.
  // Close the log file before handing off to firmware ExitBootServices.
  LogCloseFile();

  // Call the original ExitBootServices.
  // NOTE: Because we allocated pages above (EPT tables, VMCS, host stack,
  // host page table), the memory map has changed since Windows obtained
  // its MapKey.  The firmware will return EFI_INVALID_PARAMETER and Windows
  // will retry ExitBootServices via our HookedExitBootServices with a fresh
  // MapKey — that second call is handled by the g_HypervisorLaunched branch.
  ComPrint("[*] Calling original ExitBootServices (MapKey=0x");
  ComPrintHex((UINT64)MapKey);
  ComPrint(")...\r\n");

  EFI_STATUS EbsStatus = g_OriginalExitBootServices(ImageHandle, MapKey);

  // If we get here, ExitBootServices returned an error.
  // (If it succeeded, all boot services are gone and we never return here.)
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
