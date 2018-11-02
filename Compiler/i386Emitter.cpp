#include "i386Emitter.h"

#include "CompilerException.h"

namespace i386
{
    void Emitter::AsmMov(CpuRegister to, CpuRegister from, int32_t size)
    {
        switch (size) {
            case 1: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x8A;    // mov r8, rm8
                a[1] = ToXrm(3, to, from);
                break;
            }
            case 2: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x8B;    // mov r16, rm16
                a[1] = ToXrm(3, to, from);
                break;
            }
            case 4: {
                uint8_t* a = AllocateBufferForInstruction(3);
                a[0] = 0x66;    // Operand size prefix
                a[1] = 0x8B;    // mov r32, rm32
                a[2] = ToXrm(3, to, from);
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    }

    void Emitter::AsmMov(CpuRegister r16, CpuSegment sreg)
    {
        uint8_t* a = AllocateBufferForInstruction(2);
        a[0] = 0x8C;    // mov rm16, sreg
        a[1] = ToXrm(3, sreg, r16);
    }

    void Emitter::AsmMov(CpuSegment sreg, CpuRegister r16)
    {
        uint8_t* a = AllocateBufferForInstruction(2);
        a[0] = 0x8E;    // mov sreg, rm16
        a[1] = ToXrm(3, sreg, r16);
    }

    void Emitter::AsmAdd(CpuRegister to, CpuRegister from, int32_t size)
    {
        switch (size) {
            case 1: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x00;    // add rm8, r8
                a[1] = ToXrm(3, from, to);
                break;
            }
            case 2: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x01;    // add rm16, r16
                a[1] = ToXrm(3, from, to);
                break;
            }
            case 4: {
                uint8_t* a = AllocateBufferForInstruction(3);
                a[0] = 0x66;    // Operand size prefix
                a[1] = 0x01;    // add rm32, r32
                a[2] = ToXrm(3, from, to);
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    }

    void Emitter::AsmSub(CpuRegister to, CpuRegister from, int32_t size)
    {
        switch (size) {
            case 1: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x28;    // sub rm8, r8
                a[1] = ToXrm(3, from, to);
                break;
            }
            case 2: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x29;    // sub rm16, r16
                a[1] = ToXrm(3, from, to);
                break;
            }
            case 4: {
                uint8_t* a = AllocateBufferForInstruction(3);
                a[0] = 0x66;    // Operand size prefix
                a[1] = 0x29;    // sub rm32, r32
                a[2] = ToXrm(3, from, to);
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    }

    void Emitter::AsmInc(CpuRegister r, int32_t size)
    {
        switch (size) {
            case 1: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0xFE;            // inc rm8
                a[1] = ToXrm(3, 0, r);
                break;
            }
            case 2: {
                uint8_t* a = AllocateBufferForInstruction(1);
                a[0] = ToOpR(0x40, r);  // inc r16
                break;
            }
            case 4: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x66;            // Operand size prefix
                a[1] = ToOpR(0x40, r);  // inc r32
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    }

    void Emitter::AsmDec(CpuRegister r, int32_t size)
    {
        switch (size) {
            case 1: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0xFE;            // dec rm8
                a[1] = ToXrm(3, 1, r);
                break;
            }
            case 2: {
                uint8_t* a = AllocateBufferForInstruction(1);
                a[0] = ToOpR(0x48, r);  // dec r16
                break;
            }
            case 4: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x66;            // Operand size prefix
                a[1] = ToOpR(0x48, r);  // dec r32
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    }

    void Emitter::AsmOr(CpuRegister to, CpuRegister from, int32_t size)
    {
        switch (size) {
            case 1: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x08;    // or rm8, r8
                a[1] = ToXrm(3, from, to);
                break;
            }
            case 2: {
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x09;    // or rm16, r16
                a[1] = ToXrm(3, from, to);
                break;
            }
            case 4: {
                uint8_t* a = AllocateBufferForInstruction(3);
                a[0] = 0x66;    // Operand size prefix
                a[1] = 0x09;    // or rm32, r32
                a[2] = ToXrm(3, from, to);
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    }

    void Emitter::AsmProcEnter()
    {
        uint8_t* a = AllocateBufferForInstruction(2 + 3);
        a[0] = 0x66;                            // Operand size prefix
        a[1] = ToOpR(0x50, CpuRegister::BP);    // push ebp
        a[2] = 0x66;                            // Operand size prefix
        a[3] = 0x8B;                            // mov r32 (ebp), rm32 (esp)
        a[4] = ToXrm(3, CpuRegister::BP, CpuRegister::SP);
    }

    void Emitter::AsmProcLeave(uint16_t retn_imm16, bool restore_sp)
    {
        if (restore_sp) {
            // Restore stack to relative zero (base pointer)
            // Only if it was modified by the procedure
            uint8_t* a1 = AllocateBufferForInstruction(3);
            a1[0] = 0x66;                       // Operand size prefix
            a1[1] = 0x8B;                       // mov r32 (esp), rm32 (ebp)
            a1[2] = ToXrm(3, CpuRegister::SP, CpuRegister::BP);
        }

        // Pop saved base pointer
        uint8_t* a2 = AllocateBufferForInstruction(2);
        a2[0] = 0x66;                           // Operand size prefix
        a2[1] = ToOpR(0x58, CpuRegister::BP);   // pop ebp

        // Call retn
        AsmProcLeaveNoArgs(retn_imm16);
    }

    void Emitter::AsmProcLeaveNoArgs(uint16_t retn_imm16)
    {
        if (retn_imm16 > 0) {
            uint8_t* a = AllocateBufferForInstruction(1 + 2);
            a[0] = 0xC2;    // retn imm16
            *(uint16_t*)(a + 1) = retn_imm16;
        } else {
            uint8_t* a = AllocateBufferForInstruction(1);
            a[0] = 0xC3;    // retn
        }
    }

    void Emitter::AsmInt(uint8_t imm8)
    {
        uint8_t* a = AllocateBufferForInstruction(1 + 1);
        a[0] = 0xCD;    // int imm8
        a[1] = imm8;
    }

    void Emitter::AsmInt(uint8_t imm8, uint8_t ah_imm8)
    {
        uint8_t* a = AllocateBufferForInstruction(1 + 1 + 1 + 1);
        a[0] = 0xB4;    // mov ah, imm8
        a[1] = ah_imm8;
        a[2] = 0xCD;    // int imm8
        a[3] = imm8;
    }
}