#include "ComLogger.h"
#include "PhysicalMemory.h"
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <intrin.h>

static EFI_FILE_PROTOCOL* g_LogFile = NULL;
static EFI_PHYSICAL_ADDRESS g_LogBufferPhys = 0;
static UINTN g_LogBufferOffset = 0;
static BOOLEAN g_ExitBootServicesTriggered = FALSE;

VOID ComInit(VOID)
{
    // Disable all interrupts
    __outbyte(COM1_PORT + 1, 0x00);
    // Enable DLAB (set baud rate divisor)
    __outbyte(COM1_PORT + 3, 0x80);
    // Set divisor to 1 (lo byte) 115200 baud
    __outbyte(COM1_PORT + 0, 0x01);
    // (hi byte)
    __outbyte(COM1_PORT + 1, 0x00);
    // 8 bits, no parity, one stop bit
    __outbyte(COM1_PORT + 3, 0x03);
    // Enable FIFO, clear them, with 14-byte threshold
    __outbyte(COM1_PORT + 2, 0xC7);
    // IRQs enabled, RTS/DSR set
    __outbyte(COM1_PORT + 4, 0x0B);
}

VOID ComPrint(CHAR8* String)
{
    CHAR8* Ptr = String;
    
    while (*Ptr != '\0') {
        // 1. Wait for the transmit buffer to be empty, with a safe timeout!
        UINT32 Timeout = 100000;
        while ((__inbyte(COM1_PORT + 5) & 0x20) == 0) {
            Timeout--;
            if (Timeout == 0) {
                break;
            }
        }
        
        if (Timeout > 0) {
            __outbyte(COM1_PORT, *Ptr);
        }

        
        // 2. Append to circular memory buffer (HyperVenom Style)
        if (g_LogBufferPhys) {
            CHAR8* Buffer = (CHAR8*)g_LogBufferPhys;
            Buffer[g_LogBufferOffset] = *Ptr;
            g_LogBufferOffset = (g_LogBufferOffset + 1) % 4096;
            Buffer[g_LogBufferOffset] = '\0';
        }
        
        // 3. Print to UEFI Screen (ASCII to Unicode conversion)
        if (!g_ExitBootServicesTriggered && gST && gST->ConOut) {
            CHAR16 UniChar[2];
            UniChar[0] = (CHAR16)*Ptr;
            UniChar[1] = L'\0';
            gST->ConOut->OutputString(gST->ConOut, UniChar);
        }
        
        Ptr++;
    }
    
    // 4. Write to synex_boot.log file (if file logging is active)
    if (!g_ExitBootServicesTriggered && g_LogFile) {
        UINTN Length = 0;
        while (String[Length] != '\0') Length++;
        g_LogFile->Write(g_LogFile, &Length, String);
        g_LogFile->Flush(g_LogFile);
    }
}

VOID ComPrintHex(UINT64 Value)
{
    CHAR8* HexChars = "0123456789ABCDEF";
    CHAR8 Buffer[19];
    Buffer[0] = '0';
    Buffer[1] = 'x';
    for (int i = 0; i < 16; i++) {
        Buffer[17 - i] = HexChars[(Value >> (i * 4)) & 0xF];
    }
    Buffer[18] = '\0';
    ComPrint(Buffer);
}

VOID LogInitFile(EFI_HANDLE ImageHandle)
{
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
    
    Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
    if (EFI_ERROR(Status) || !LoadedImage) return;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    Status = gBS->HandleProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Fs);
    if (EFI_ERROR(Status) || !Fs) return;

    EFI_FILE_PROTOCOL *Root = NULL;
    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status) || !Root) return;

    // Open synex_boot.log (Create or Overwrite fresh)
    Status = Root->Open(Root, &g_LogFile, L"synex_boot.log", EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(Status)) {
        g_LogFile = NULL;
    } else {
        // Clear old logs by recreating a fresh file
        g_LogFile->Delete(g_LogFile);
        
        Status = Root->Open(Root, &g_LogFile, L"synex_boot.log", EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
        if (EFI_ERROR(Status)) {
            g_LogFile = NULL;
        } else {
            UINTN Size = 36;
            g_LogFile->Write(g_LogFile, &Size, "--- SynexUefi Boot Log Started ---\r\n");
            g_LogFile->Flush(g_LogFile);
        }
    }
}

VOID LogInitBuffer(VOID)
{
    g_LogBufferPhys = MemAllocatePages(1);
    if (g_LogBufferPhys) {
        g_LogBufferOffset = 0;
        CHAR8* Buffer = (CHAR8*)g_LogBufferPhys;
        Buffer[0] = '\0';
    }
}

VOID LogCloseFile(VOID)
{
    g_ExitBootServicesTriggered = TRUE;
    g_LogFile = NULL;
}


EFI_PHYSICAL_ADDRESS LogGetBufferAddress(VOID)
{
    return g_LogBufferPhys;
}
