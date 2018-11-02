#include "SuppressRegister.h"

SuppressRegister::SuppressRegister(DosExeEmitter* emitter, i386::CpuRegister reg)
    : emitter(emitter),
      reg(reg)
{
    emitter->suppressed_registers.insert(reg);
}

SuppressRegister::~SuppressRegister()
{
    emitter->suppressed_registers.erase(reg);
}