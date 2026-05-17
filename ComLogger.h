#ifndef COM_LOGGER_H
#define COM_LOGGER_H

#include <Uefi.h>

#define COM1_PORT 0x3F8

// Main log functions
VOID ComInit(VOID);
VOID ComPrint(CHAR8* String);
VOID ComPrintHex(UINT64 Value);

// Advanced Logging Extensions (Screen, File, Buffer)
VOID LogInitFile(EFI_HANDLE ImageHandle);
VOID LogInitBuffer(VOID);
VOID LogCloseFile(VOID);
EFI_PHYSICAL_ADDRESS LogGetBufferAddress(VOID);

#endif // COM_LOGGER_H
