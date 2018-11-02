#pragma once

#include "GenericEmitter.h"

namespace i386
{
    /// <summary>
    /// All available general-purpose registers
    /// </summary>
    enum struct CpuRegister {
        None = 0xFF,

        AL = 0,
        CL = 1,
        DL = 2,
        BL = 3,
        AH = 4,
        CH = 5,
        DH = 6,
        BH = 7,

        AX = 0,
        CX = 1,
        DX = 2,
        BX = 3,
        SP = 4,
        BP = 5,
        SI = 6,
        DI = 7
    };

    /// <summary>
    /// All available segment registers
    /// </summary>
    enum struct CpuSegment {
        ES = 0,
        CS = 1,
        SS = 2,
        DS = 3,
        FS = 4,
        GS = 5
    };

    /// <summary>
    /// Class that emits machine code for i386 architecture
    /// </summary>
    class Emitter : protected GenericEmitter
    {

    protected:
        void AsmMov(CpuRegister to, CpuRegister from, int32_t size);
        void AsmMov(CpuRegister r16, CpuSegment sreg);
        void AsmMov(CpuSegment sreg, CpuRegister r16);

        void AsmAdd(CpuRegister to, CpuRegister from, int32_t size);
        void AsmSub(CpuRegister to, CpuRegister from, int32_t size);
        void AsmInc(CpuRegister r, int32_t size);
        void AsmDec(CpuRegister r, int32_t size);
        void AsmOr(CpuRegister to, CpuRegister from, int32_t size);

        /// <summary>
        /// Emit instructions for the start of a procedure
        /// </summary>
        void AsmProcEnter();

        /// <summary>
        /// Emit instructions for the end of a procesure
        /// </summary>
        /// <param name="retn_imm16">Size of parameters (number of bytes to release from the stack)</param>
        /// <param name="restore_sp">Restore stack (not necessary if stack was not used)</param>
        void AsmProcLeave(uint16_t retn_imm16, bool restore_sp = false);

        /// <summary>
        /// Emit instructions for the end of a procesure without restoring stack and base pointer
        /// Stack has to stay untouched, because stack pointer is not saved
        /// </summary>
        /// <param name="retn_imm16">Size of parameters (number of bytes to release from the stack)</param>
        void AsmProcLeaveNoArgs(uint16_t retn_imm16);

        /// <summary>
        /// Emit software interrupt
        /// </summary>
        /// <param name="imm8">Interrupt number</param>
        void AsmInt(uint8_t imm8);

        /// <summary>
        /// Copy constant to AH register and emit software interrupt
        /// AH register will be overwritten
        /// </summary>
        /// <param name="imm8">Interrupt number</param>
        /// <param name="ah_imm8">AH value</param>
        void AsmInt(uint8_t imm8, uint8_t ah_imm8);

        template<typename TR>
        constexpr uint8_t ToOpR(uint8_t op, TR r) const
        {
            return (uint8_t)(op + ((uint8_t)(r) & 0x07));
        }

        template<typename TR, typename TM>
        constexpr uint8_t ToXrm(uint8_t x, TR r, TM m) const
        {
            return (uint8_t)((((uint8_t)(x) << 6) & 0xC0) | (((uint8_t)(r) << 3) & 0x38) | ((uint8_t)(m) & 0x07));
        }

    };
}