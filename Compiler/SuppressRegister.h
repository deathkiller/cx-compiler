#pragma once

#include "DosExeEmitter.h"

/// <summary>
/// Specified register will be preserved and not used by automatic register allocation
/// </summary>
class SuppressRegister
{
public:
    SuppressRegister(DosExeEmitter* emitter, i386::CpuRegister reg);
    ~SuppressRegister();

private:
    DosExeEmitter* emitter;
    i386::CpuRegister reg;
};