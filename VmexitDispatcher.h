#ifndef VMEXIT_DISPATCHER_H
#define VMEXIT_DISPATCHER_H

#include <Uefi.h>
#include "GuestRegisters.h"

BOOLEAN VmexitDispatcher(PGUEST_REGISTERS GuestRegs);

#endif
