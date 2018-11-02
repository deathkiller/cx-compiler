#include "DosExeEmitter.h"

#include <iostream>
#include <list>
#include <memory>
#include <stack>
#include <functional>
#include <unordered_set>

#include "Log.h"
#include "Compiler.h"
#include "CompilerException.h"
#include "SuppressRegister.h"

// This emitter is using i386 architecture
using namespace i386;

DosExeEmitter::DosExeEmitter(Compiler* compiler)
    : compiler(compiler)
{
}

DosExeEmitter::~DosExeEmitter()
{
    std::unordered_set<char*>::iterator it = strings.begin();

    while (it != strings.end()) {
        free(*it);

        ++it;
    }

    strings.clear();
}

void DosExeEmitter::EmitMzHeader()
{
    int32_t header_size = sizeof(MzHeader);

    MzHeader* header = AllocateBuffer<MzHeader>();
    memset(header, 0, header_size);

    // Write valid signature
    header->signature[0] = 'M';
    header->signature[1] = 'Z';
    header->header_paragraphs = ((header_size + 16 - 1) >> 4);

    // Fill the remaining space, so instructions are aligned
    int32_t remaining = (header->header_paragraphs << 4) - header_size;
    if (remaining > 0) {
        uint8_t* fill = AllocateBuffer(remaining);
        memset(fill, 0, remaining);
    }
}

void DosExeEmitter::EmitInstructions(InstructionEntry* instruction_stream)
{
    Log::Write(LogType::Info, "Compiling intermediate code to i386 machine code...");
    Log::PushIndent();
    Log::PushIndent();

    SymbolTableEntry* symbol_table = compiler->GetSymbols();

    CreateVariableList(symbol_table);

    // Find IPs that are targets for "goto" statements,
    // at these places, compiler must unload all variables from registers
    std::unordered_set<uint32_t> discontinuous_ips;
    {
        InstructionEntry* current = instruction_stream;
        while (current) {
            if (current->type == InstructionType::Goto) {
                discontinuous_ips.insert(current->goto_statement.ip);
            } else if (current->type == InstructionType::If) {
                discontinuous_ips.insert(current->if_statement.ip);
            }

            current = current->next;
        }
    }

    std::stack<InstructionEntry*> call_parameters;

    current_instruction = instruction_stream;

    if (current_instruction && current_instruction->type == InstructionType::Goto) {
        // Skip first "goto" instruction
        current_instruction = current_instruction->next;
        ip_src++;
    }

    while (current_instruction) {

        // Unload all registers before "goto" statement target, so we can
        // jump to it without any issues
        if (discontinuous_ips.find(ip_src) != discontinuous_ips.end()) {
            SaveAndUnloadAllRegisters(SaveReason::Before);
        }

        // Used for abstract instruction to real instruction pointer conversion
        ip_src_to_dst[ip_src] = ip_dst;

        // These methods are called before every abstract instruction
        ProcessSymbolLinkage(symbol_table);

        if (!current_instruction) {
            // Current instruction can be changed by ProcessSymbolLinkage
            break;
        }

        BackpatchAddresses();

        was_return = false;

        switch (current_instruction->type) {
            case InstructionType::Nop:        break;
            case InstructionType::Assign:     EmitAssign(current_instruction);                  break;
            case InstructionType::Goto:       EmitGoto(current_instruction);                    break;
            case InstructionType::GotoLabel:  EmitGotoLabel(current_instruction);               break;
            case InstructionType::If:         EmitIf(current_instruction);                      break;
            case InstructionType::Push:       EmitPush(current_instruction, call_parameters);   break;
            case InstructionType::Call:       EmitCall(current_instruction, symbol_table, call_parameters); break;
            case InstructionType::Return:     EmitReturn(current_instruction, symbol_table);    break;

            default: ThrowOnUnreachableCode();
        }

        current_instruction = current_instruction->next;
        ip_src++;
    }

    EmitFunctionEpilogue();

    Log::PopIndent();
    Log::PopIndent();
}

void DosExeEmitter::EmitSharedFunctions()
{
    Log::Write(LogType::Info, "Emitting shared functions...");
    Log::PushIndent();

    SymbolTableEntry* symbol_table = compiler->GetSymbols();

    // This buffer is used for (almost) all I/O operations
    const int32_t io_buffer_size = 0x20; // 32 bytes

    bool io_buffer_needed = false;
    uint16_t io_buffer_address = 0;

    // Check, if I/O buffer for read/print operations is needed
    {
        SymbolTableEntry* current = symbol_table;

        while (current) {
            if (current->type.base == BaseSymbolType::SharedFunction && current->ref_count > 0) {
                // Process only referenced shared functions
                if (strcmp(current->name, "PrintUint32") == 0  ||
                    strcmp(current->name, "PrintNewLine") == 0 ||
                    strcmp(current->name, "ReadUint32") == 0) {

                    io_buffer_needed = true;
                    break;
                }
            }

            current = current->next;
        }
    }

    // Buffer is needed, allocate space for it
    if (io_buffer_needed) {
        io_buffer_address = ip_dst + 0x0100 /*Program Segment Prefix*/;

        // ToDo: This could be allocated only on runtime
        uint8_t* buffer = AllocateBufferForInstruction(io_buffer_size);
        memset(buffer, 0, io_buffer_size);
    }

    // Emit only referenced functions
    EmitSharedFunction("PrintUint32", [&]() {
        AsmProcEnter();

        //   mov eax, ss:[bp + 6]
        uint8_t* l2 = AllocateBufferForInstruction(4);
        l2[0] = 0x66;   // Operand size prefix
        l2[1] = 0x8B;   // mov r32, rm32
        l2[2] = ToXrm(1, CpuRegister::AX, 6);
        l2[3] = (int8_t)6;

        LoadConstantToRegister(10, CpuRegister::CX, 4);
        LoadConstantToRegister(20, CpuRegister::DI, 2);

        //   mov [buffer + DI], '$'
        uint8_t* l5 = AllocateBufferForInstruction(2 + 2 + 1);
        l5[0] = 0xC6;  // mov rm8, imm8
        l5[1] = ToXrm(2, 0, 5);
        *(uint16_t*)(l5 + 2) = io_buffer_address;
        l5[4] = 0x24; // '$'

        uint32_t loop = ip_dst;

        AsmDec(CpuRegister::DI, 2);

        ZeroRegister(CpuRegister::DX, 4);

        //   div ecx
        uint8_t* l7 = AllocateBufferForInstruction(3);
        l7[0] = 0x66;   // Operand size prefix
        l7[1] = 0xF7;   // div eax, rm32
        l7[2] = ToXrm(3, 6, CpuRegister::CX);

        //   add dl, '0'
        uint8_t* l8 = AllocateBufferForInstruction(2 + 1);
        l8[0] = 0x80;   // add rm8, imm8
        l8[1] = ToXrm(3, 0, CpuRegister::DL);
        l8[2] = '0';

        //   mov [buffer + DI], dl
        uint8_t* l9 = AllocateBufferForInstruction(2 + 2);
        l9[0] = 0x88;   // mov rm8, r8
        l9[1] = ToXrm(2, CpuRegister::DL, 5);
        *(uint16_t*)(l9 + 2) = io_buffer_address;

        //   cmp eax, 0
        uint8_t* l10 = AllocateBufferForInstruction(4);
        l10[0] = 0x66;   // Operand size prefix
        l10[1] = 0x83;   // cmp rm32, imm8
        l10[2] = ToXrm(3, 7, CpuRegister::AX);
        l10[3] = 0;

        //   jnz [loop]
        uint8_t* l11 = AllocateBufferForInstruction(1 + 1);
        l11[0] = 0x75;   // jnz rel8
        *(int8_t*)(l11 + 1) = (int8_t)(loop - ip_dst);

        LoadConstantToRegister(io_buffer_address, CpuRegister::DX, 2);

        AsmAdd(CpuRegister::DX, CpuRegister::DI, 2);

        AsmInt(0x21 /*DOS Function Dispatcher*/, 0x09 /*Write String To Stdout*/);

        AsmProcLeave(4);
    });

    EmitSharedFunction("PrintString", [&]() {
        AsmProcEnter();

        //   mov dx, ss:[bp + 6]
        uint8_t* l2 = AllocateBufferForInstruction(3);
        l2[0] = 0x8B;   // mov r16, rm16
        l2[1] = ToXrm(1, CpuRegister::DX, 6);
        l2[2] = (int8_t)6;

        AsmMov(CpuRegister::SI, CpuRegister::DX, 2);

        uint32_t loop = ip_dst;

        //   mov bl, [SI]
        uint8_t* l4 = AllocateBufferForInstruction(2);
        l4[0] = 0x8A;   // mov r8, rm8
        l4[1] = ToXrm(0, CpuRegister::BL, 4);

        AsmInc(CpuRegister::SI, 2);

        AsmOr(CpuRegister::BL, CpuRegister::BL, 1);

        //   jnz [loop]
        uint8_t* l7 = AllocateBufferForInstruction(2);
        l7[0] = 0x75;   // jnz rel8
        l7[1] = (int8_t)(loop - ip_dst);

        AsmDec(CpuRegister::SI, 2);

        //   mov [SI], '$'
        uint8_t* l9 = AllocateBufferForInstruction(3);
        l9[0] = 0xC6;   // mov rm8, imm8
        l9[1] = ToXrm(0, 0, 4);
        l9[2] = '$';

        AsmInt(0x21 /*DOS Function Dispatcher*/, 0x09 /*Write String To Stdout*/);

        //   mov [SI], bl
        uint8_t* l11 = AllocateBufferForInstruction(2);
        l11[0] = 0x88;   // mov rm8, r8
        l11[1] = ToXrm(0, CpuRegister::BL, 4);

        AsmProcLeave(2);
    });

    EmitSharedFunction("PrintNewLine", [&]() {
        //   mov [buffer], '\r\n$\0'
        uint8_t* l1 = AllocateBufferForInstruction(3 + 2 + 4);
        l1[0] = 0x66;   // Operand size prefix
        l1[1] = 0xC7;   // mov rm32, imm32
        l1[2] = ToXrm(0, 0, 6);
        *(uint16_t*)(l1 + 3) = io_buffer_address;
        *(uint32_t*)(l1 + 5) = 0x00240A0D;      // '\r\n$\0'

        LoadConstantToRegister(io_buffer_address, CpuRegister::DX, 2);

        AsmInt(0x21 /*DOS Function Dispatcher*/, 0x09 /*Write String To Stdout*/);

        AsmProcLeaveNoArgs(0);
    });

    EmitSharedFunction("ReadUint32", [&]() {
        //   mov [buffer], <buffer_size, 0>
        uint8_t* l1 = AllocateBufferForInstruction(2 + 2 + 2);
        l1[0] = 0xC7;   // mov rm16, imm16
        l1[1] = ToXrm(0, 0, 6);
        *(uint16_t*)(l1 + 2) = io_buffer_address;
        *(uint16_t*)(l1 + 4) = io_buffer_size;

        LoadConstantToRegister(io_buffer_address, CpuRegister::DX, 2);

        AsmInt(0x21 /*DOS Function Dispatcher*/, 0x0A /*Buffered Keyboard Input*/);

        ZeroRegister(CpuRegister::AX, 4);
        ZeroRegister(CpuRegister::BX, 4);

        LoadConstantToRegister(2, CpuRegister::SI, 2);
        LoadConstantToRegister(10, CpuRegister::CX, 4);

        uint32_t loop = ip_dst;

        //   mov bl, [buffer + SI]
        uint8_t* l6 = AllocateBufferForInstruction(2 + 2);
        l6[0] = 0x8A;   // mov r8, rm8
        l6[1] = ToXrm(2, CpuRegister::BL, 4);
        *(uint16_t*)(l6 + 2) = io_buffer_address;

        //   cmp bl, '9'
        uint8_t* l7 = AllocateBufferForInstruction(2 + 1);
        l7[0] = 0x80;   // cmp rm8, imm8
        l7[1] = ToXrm(3, 7, CpuRegister::BL);
        l7[2] = '9';

        //   ja [end]
        uint8_t* l9 = AllocateBufferForInstruction(1 + 1);
        l9[0] = 0x77;   // ja rel8

        uint32_t l9_ip = ip_dst;
        uint32_t l9_offset = (uint32_t)((l9 + 1) - buffer);

        //   sub bl, '0'
        uint8_t* l10 = AllocateBufferForInstruction(2 + 1);
        l10[0] = 0x80;  // sub rm8, imm8
        l10[1] = ToXrm(3, 5, CpuRegister::BL);
        l10[2] = '0';

        //   jb [end]
        uint8_t* l11 = AllocateBufferForInstruction(1 + 1);
        l11[0] = 0x72;  // jb rel8

        uint32_t l11_ip = ip_dst;
        uint32_t l11_offset = (uint32_t)((l11 + 1) - buffer);

        //   mul ecx
        uint8_t* l12 = AllocateBufferForInstruction(3);
        l12[0] = 0x66;  // Operand size prefix
        l12[1] = 0xF7;  // mul eax, rm32
        l12[2] = ToXrm(3, 4, CpuRegister::CX);

        AsmAdd(CpuRegister::AX, CpuRegister::BX, 4);

        AsmInc(CpuRegister::SI, 2);

        //   jmp [loop]
        uint8_t* l15 = AllocateBufferForInstruction(1 + 1);
        l15[0] = 0xEB;  // jmp rel8
        l15[1] = (int8_t)(loop - ip_dst);

        // Backpatch "end" jumps, offset is known now
        uint32_t end = ip_dst;
        *(buffer + l9_offset) = (int8_t)(end - l9_ip);
        *(buffer + l11_offset) = (int8_t)(end - l11_ip);

        AsmProcLeaveNoArgs(0);
    });

    EmitSharedFunction("GetCommandLine", [&]() {
        //   mov si, (0x81 - 1)
        LoadConstantToRegister(0x81 - 1, CpuRegister::SI, 2);

        uint32_t loop1 = ip_dst;

        // Go forward and find first non-whitespace character
        //   inc si
        uint8_t* l3 = AllocateBufferForInstruction(1);
        l3[0] = ToOpR(0x40, CpuRegister::SI);   // inc r16

        //   cmp [si], ' '
        uint8_t* l4 = AllocateBufferForInstruction(2 + 1);
        l4[0] = 0x80;   // cmp rm8, imm8
        l4[1] = ToXrm(0, 7, 4);
        l4[2] = ' ';

        //   jz [loop1]
        uint8_t* l5 = AllocateBufferForInstruction(1 + 1);
        l5[0] = 0x74;   // jz rel8
        l5[1] = (int8_t)(loop1 - ip_dst);

        // Save starting address to AX
        AsmMov(CpuRegister::AX, CpuRegister::SI, 2);

        AsmDec(CpuRegister::SI, 2);

        uint32_t loop2 = ip_dst;

        // Go forward and find CR
        AsmInc(CpuRegister::SI, 2);

        //   cmp [si], '\r'
        uint8_t* l9 = AllocateBufferForInstruction(2 + 1);
        l9[0] = 0x80;   // cmp rm8, imm8
        l9[1] = ToXrm(0, 7, 4);
        l9[2] = '\r';

        //   jnz [loop2]
        uint8_t* l10 = AllocateBufferForInstruction(1 + 1);
        l10[0] = 0x75;   // jnz rel8
        l10[1] = (int8_t)(loop2 - ip_dst);

        uint32_t loop3 = ip_dst;

        // Go backward and find first non-whitespace character
        AsmDec(CpuRegister::SI, 2);

        //   cmp [si], ' '
        uint8_t* l12 = AllocateBufferForInstruction(2 + 1);
        l12[0] = 0x80;   // cmp rm8, imm8
        l12[1] = ToXrm(0, 7, 4);
        l12[2] = ' ';

        //   jz [loop3]
        uint8_t* l13 = AllocateBufferForInstruction(1 + 1);
        l13[0] = 0x74;   // jz rel8
        l13[1] = (int8_t)(loop3 - ip_dst);

        AsmInc(CpuRegister::SI, 2);

        //   mov [si], '\0'
        uint8_t* l15 = AllocateBufferForInstruction(2 + 1);
        l15[0] = 0xC6;   // mov rm8, imm8
        l15[1] = ToXrm(0, 0, 4);
        l15[2] = 0x00;   // '\0'

        AsmProcLeaveNoArgs(0);
    });

    EmitSharedFunction("#StringsEqual", [&]() {
        AsmProcEnter();

        //   mov si, ss:[bp + 6]
        uint8_t* l2 = AllocateBufferForInstruction(3);
        l2[0] = 0x8B;   // mov r16, rm16
        l2[1] = ToXrm(1, CpuRegister::SI, 6);
        l2[2] = (int8_t)6;

        //   mov di, ss:[bp + 8]
        uint8_t* l3 = AllocateBufferForInstruction(3);
        l3[0] = 0x8B;   // mov r16, rm16
        l3[1] = ToXrm(1, CpuRegister::DI, 6);
        l3[2] = (int8_t)8;

        //   cmp si, di
        uint8_t* l4 = AllocateBufferForInstruction(2);
        l4[0] = 0x39;   // cmp rm16, r16
        l4[1] = ToXrm(3, CpuRegister::DI, CpuRegister::SI);

        //   jz [equal]
        uint8_t* l5 = AllocateBufferForInstruction(1 + 1);
        l5[0] = 0x74;   // jz rel8

        uint32_t l5_ip = ip_dst;
        uint32_t l5_offset = (uint32_t)((l5 + 1) - buffer);

        AsmDec(CpuRegister::DI, 2);

        uint8_t loop = ip_dst;

        AsmInc(CpuRegister::DI, 2);

        //   lodsb
        uint8_t* l8 = AllocateBufferForInstruction(1);
        l8[0] = 0xAC;

        //   cmp [di], al
        uint8_t* l9 = AllocateBufferForInstruction(2);
        l9[0] = 0x38;   // cmp rm8, r8
        l9[1] = ToXrm(0, CpuRegister::AL, 5);

        //   jnz [not_equal]
        uint8_t* l10 = AllocateBufferForInstruction(1 + 1);
        l10[0] = 0x75;  // jnz rel8
        
        uint32_t l10_ip = ip_dst;
        uint32_t l10_offset = (uint32_t)((l10 + 1) - buffer);

        //   cmp al, 0
        uint8_t* l11 = AllocateBufferForInstruction(2 + 1);
        l11[0] = 0x80;  // cmp rm8, imm8
        l11[1] = ToXrm(3, 7, CpuRegister::AL);
        l11[2] = 0;

        //   jnz [loop]
        uint8_t* l12 = AllocateBufferForInstruction(1 + 1);
        l12[0] = 0x75;  // jnz rel8
        l12[1] = (int8_t)(loop - ip_dst);

        // They are equal
        uint32_t equal = ip_dst;
        *(buffer + l5_offset) = (int8_t)(equal - l5_ip);

        LoadConstantToRegister(1, CpuRegister::AL, 1);

        //   jmp [end]
        uint8_t* l14 = AllocateBufferForInstruction(1 + 1);
        l14[0] = 0xEB;  // jmp rel8

        uint32_t l14_ip = ip_dst;
        uint32_t l14_offset = (uint32_t)((l14 + 1) - buffer);

        // They are not equal
        // Backpatch "not_equal" jump, offset is known now
        uint32_t not_equal = ip_dst;
        *(buffer + l10_offset) = (int8_t)(not_equal - l10_ip);

        ZeroRegister(CpuRegister::AL, 1);

        // Backpatch "end" jump, offset is known now
        uint32_t end = ip_dst;
        *(buffer + l14_offset) = (int8_t)(end - l14_ip);

        AsmProcLeave(4);
    });

    // ToDo: Check (ptr + bytes) is fully accessible; if it's not, release and return null
    EmitSharedFunction("#Alloc", [&]() {
        AsmProcEnter();

        //   mov ebx, ss:[bp + 6]
        uint8_t* l2 = AllocateBufferForInstruction(4);
        l2[0] = 0x66;   // Operand size prefix
        l2[1] = 0x8B;   // mov r16, rm16
        l2[2] = ToXrm(1, CpuRegister::BX, 6);
        l2[3] = (int8_t)6;

        // To check parameter by 'jz' instruction
        //   or bx, bx
        AsmOr(CpuRegister::BX, CpuRegister::BX, 2);

        //   jz [ret_null]
        uint8_t* l4 = AllocateBufferForInstruction(2);
        l4[0] = 0x74;   // jz rel8

        uint32_t l4_ip = ip_dst;
        uint32_t l4_offset = (uint32_t)((l4 + 1) - buffer);

        // Cannot allocate more than 64k bytes
        //   test ebx, FFFF0000h
        uint8_t* l5 = AllocateBufferForInstruction(3 + 4);
        l5[0] = 0x66;   // Operand size prefix
        l5[1] = 0xF7;   // test rm32, imm32
        l5[2] = ToXrm(3, 0, CpuRegister::BX);
        *(uint32_t*)(l5 + 3) = 0xffff0000;

        //   jnz [ret_null]
        uint8_t* l6 = AllocateBufferForInstruction(2);
        l6[0] = 0x75;   // jnz rel8

        uint32_t l6_ip = ip_dst;
        uint32_t l6_offset = (uint32_t)((l6 + 1) - buffer);

        // Convert bytes to paragraphs with round up
        //   add bx, 15
        uint8_t* l7 = AllocateBufferForInstruction(2 + 2);
        l7[0] = 0x81;  // add rm16, imm16
        l7[1] = ToXrm(3, 0, CpuRegister::BX);
        *(uint16_t*)(l7 + 2) = 15;

        //   shr bx, 4
        uint8_t* l8 = AllocateBufferForInstruction(3);
        l8[0] = 0xC1;  // shr rm16, imm8
        l8[1] = ToXrm(3, 5, CpuRegister::BX);
        l8[2] = 4;

        AsmInt(0x21 /*DOS Function Dispatcher*/, 0x48 /*Allocate Memory*/);

        // Allocation failed
        //   jb/jc [ret_null]
        uint8_t* l10 = AllocateBufferForInstruction(2);
        l10[0] = 0x72;  // jc rel8

        uint32_t l10_ip = ip_dst;
        uint32_t l10_offset = (uint32_t)((l10 + 1) - buffer);

        // Check for 16-bit segment overflow
        //   test ax, F000h
        uint8_t* l11 = AllocateBufferForInstruction(2 + 2);
        l11[0] = 0xF7;  // test rm16, imm16
        l11[1] = ToXrm(3, 0, CpuRegister::AX);
        *(uint16_t*)(l11 + 2) = 0xf000;

        //   jnz [ret_null]
        uint8_t* l12 = AllocateBufferForInstruction(2);
        l12[0] = 0x75;  // jnz rel8

        uint32_t l12_ip = ip_dst;
        uint32_t l12_offset = (uint32_t)((l12 + 1) - buffer);

        // Backup allocated segment
        //   mov cx, ax
        AsmMov(CpuRegister::CX, CpuRegister::AX, 2);

        //   mov bx, ds
        AsmMov(CpuRegister::BX, CpuSegment::DS);

        //   sub ax, bx
        AsmSub(CpuRegister::AX, CpuRegister::BX, 2);

        // Segment too far to use
        //   jb [ret_null]
        uint8_t* l15 = AllocateBufferForInstruction(2);
        l15[0] = 0x72;  // jb rel8

        uint32_t l15_ip = ip_dst;
        uint32_t l15_offset = (uint32_t)((l15 + 1) - buffer);

        // Convert segment to pointer
        //   shl ax, 4
        uint8_t* l16 = AllocateBufferForInstruction(3);
        l16[0] = 0xC1;  // shl rm16, imm8
        l16[1] = ToXrm(3, 4, CpuRegister::AX);
        l16[2] = 4;

        //   jmp [ret_ptr]
        uint8_t* l17 = AllocateBufferForInstruction(2);
        l17[0] = 0xEB;  // jmp rel8

        uint32_t l17_ip = ip_dst;
        uint32_t l17_offset = (uint32_t)((l17 + 1) - buffer);

    // restore_release_and_ret_null:
        uint32_t restore_release_and_ret_null = ip_dst;
        *(buffer + l15_offset) = (int8_t)(restore_release_and_ret_null - l15_ip);

        // Restore backup
        //   mov ax, cx
        AsmMov(CpuRegister::AX, CpuRegister::CX, 2);

    // release_and_ret_null:
        uint32_t release_and_ret_null = ip_dst;
        *(buffer + l12_offset) = (int8_t)(release_and_ret_null - l12_ip);

        // Backup ES segment
        //   mov cx, es
        AsmMov(CpuRegister::CX, CpuSegment::ES);

        // Move it to right register
        //   mov es, ax
        AsmMov(CpuSegment::ES, CpuRegister::AX);

        AsmInt(0x21 /*DOS Function Dispatcher*/, 0x49 /*Free Allocated Memory*/);

        // Restore ES segment
        //   mov es, cx
        AsmMov(CpuSegment::ES, CpuRegister::CX);

    // ret_null:
        uint32_t ret_null = ip_dst;
        *(buffer + l4_offset) = (int8_t)(ret_null - l4_ip);
        *(buffer + l6_offset) = (int8_t)(ret_null - l6_ip);
        *(buffer + l10_offset) = (int8_t)(ret_null - l10_ip);

        ZeroRegister(CpuRegister::AX, 2);

    // ret_ptr:
        uint32_t ret_ptr = ip_dst;
        *(buffer + l17_offset) = (int8_t)(ret_ptr - l17_ip);

        AsmProcLeave(2);
    });

    EmitSharedFunction("release", [&]() {
        AsmProcEnter();

        // Load parameter (bytes)
        //   mov ax, ss:[bp + 6]
        uint8_t* l2 = AllocateBufferForInstruction(3);
        l2[0] = 0x8B;   // mov r16, rm16
        l2[1] = ToXrm(1, CpuRegister::AX, 6);
        l2[2] = (int8_t)6;

        // Convert pointer to segment
        //   shr ax, 4
        uint8_t* l3 = AllocateBufferForInstruction(3);
        l3[0] = 0xC1;  // shr rm16, imm8
        l3[1] = ToXrm(3, 5, CpuRegister::AX);
        l3[2] = 4;

        // Backup ES segment
        AsmMov(CpuRegister::CX, CpuSegment::ES);

        // Load DS segment
        AsmMov(CpuRegister::BX, CpuSegment::DS);

        // Add DS segment to our segment to release
        AsmAdd(CpuRegister::AX, CpuRegister::BX, 2);

        // Move it to right register
        AsmMov(CpuSegment::ES, CpuRegister::AX);

        AsmInt(0x21 /*DOS Function Dispatcher*/, 0x49 /*Free Allocated Memory*/);

        // Restore ES segment
        AsmMov(CpuSegment::ES, CpuRegister::CX);

        AsmProcLeave(2);
    });

    Log::PopIndent();
}

void DosExeEmitter::EmitStaticData()
{
    // Emit all unique strings, and backpatch their addresses
    {
        std::unordered_set<char*>::iterator it = strings.begin();

        while (it != strings.end()) {
            BackpatchLabels({ *it, ip_dst }, DosBackpatchTarget::String);

            uint32_t str_length = (uint32_t)strlen(*it);
            uint8_t* dst = AllocateBufferForInstruction(str_length + 1);
            memcpy(dst, *it, str_length);
            dst[str_length] = '\0';

            ++it;
        }
    }

    // Pre-allocate virtual space for all static variables
    {
        std::list<DosVariableDescriptor>::iterator it = variables.begin();

        while (it != variables.end()) {
            if (!it->symbol->parent) {
                int32_t size;
                if (it->symbol->size > 0) {
                    SymbolType resolved_type = it->symbol->type;
                    resolved_type.pointer--;
                    size = it->symbol->size * compiler->GetSymbolTypeSize(resolved_type);
                } else {
                    size = compiler->GetSymbolTypeSize(it->symbol->type);
                }

                BackpatchLabels({ it->symbol->name, ip_dst + static_size }, DosBackpatchTarget::Static);

                static_size += size;
            }

            ++it;
        }
    }
}

void DosExeEmitter::FixMzHeader(InstructionEntry* instruction_stream, uint32_t stack_size)
{
    MzHeader* header = (MzHeader*)buffer;

    Log::Write(LogType::Info, "Finalizing executable file...");
    Log::PushIndent();

    Log::Write(LogType::Verbose, "Program size: %d bytes", ip_dst);
    Log::Write(LogType::Verbose, "Static size: %d bytes", static_size);

    // Compute image size
    header->block_count = (ip_dst / 512);
    header->last_block_size = (ip_dst % 512);
    if (header->last_block_size > 0) {
        header->block_count++;
    }

    // Create stack
    header->ss = ((ip_dst + static_size + 16 - 1) >> 4);

    if (stack_size >= 0x20 /*32B*/ && stack_size <= 0x8000 /*32kB*/) {
        header->sp = stack_size;
    } else {
        header->sp = 0x2000; // 8kB is default stack size
    }

    Log::Write(LogType::Verbose, "Stack size: %d bytes", header->sp);
    Log::Write(LogType::Verbose, "Stack segment: 0x%04x", header->ss);

    // Compute additional memory needed
    header->min_extra_paragraphs = ((static_size + header->sp + 16 - 1) >> 4) + 1;
    header->max_extra_paragraphs = header->min_extra_paragraphs;

    // Adjust SP for flat memory model
    header->sp += (header->ss << 4);
    header->sp += 0x0100; // Program Segment Prefix
    header->ss = 0;

    // Adjust start IP
    if (instruction_stream && instruction_stream->type == InstructionType::Goto) {
        header->ip = ip_src_to_dst[instruction_stream->goto_statement.ip];
    }

    Log::Write(LogType::Verbose, "Entry point: 0x%04x", header->ip);

    Log::PopIndent();
}

void DosExeEmitter::Save(FILE* stream)
{
    if (buffer) {
        if (buffer_offset > 0) {
            CheckBackpatchListIsEmpty(DosBackpatchTarget::Function);
            CheckBackpatchListIsEmpty(DosBackpatchTarget::String);
            CheckBackpatchListIsEmpty(DosBackpatchTarget::Static);

            if (!fwrite(buffer, buffer_offset, 1, stream)) {
                Log::Write(LogType::Error, "Emitting of executable file failed.");
            }
        }

        free(buffer);
        buffer = nullptr;
    }
}

void DosExeEmitter::CreateVariableList(SymbolTableEntry* symbol_table)
{
    SymbolTableEntry* current = symbol_table;

    while (current) {
        if (TypeIsValid(current->type)) {
            // Add all variables to the list, so they
            // can be referenced by the compiler
            DosVariableDescriptor variable { };
            variable.symbol = current;
            variable.reg = CpuRegister::None;
            variables.push_back(variable);
        }

        current = current->next;
    }
}

CpuRegister DosExeEmitter::GetUnusedRegister()
{
    // First four 32-bit registers are generally usable
    DosVariableDescriptor* register_used[4] { };

    std::list<DosVariableDescriptor>::iterator it = variables.begin();

    while (it != variables.end()) {
        if (it->reg != CpuRegister::None && (!it->symbol->parent || (it->symbol->parent && strcmp(it->symbol->parent, parent->name) == 0))) {
            register_used[(int32_t)it->reg] = &(*it);
        }

        ++it;
    }

    DosVariableDescriptor* last_used = nullptr;

    for (int32_t i = 0; i < 4; i++) {
        if (suppressed_registers.find((CpuRegister)i) != suppressed_registers.end()) {
            // Skip suppressed registers
            continue;
        }

        if (!register_used[i]) {
            // Register is empty (it was not used yet in this scope)
            return (CpuRegister)i;
        }

        if (!last_used || last_used->last_used > register_used[i]->last_used) {
            last_used = register_used[i];
        }
    }

    CpuRegister reg = last_used->reg;

    // Register was used, save it back to the stack and discard it
    if (last_used->reg != CpuRegister::None) {
        SaveVariable(last_used, SaveReason::Inside);
    }

    last_used->reg = CpuRegister::None;
    last_used->is_dirty = false;

    return reg;
}

CpuRegister DosExeEmitter::TryGetUnusedRegister()
{
    // First four 32-bit registers are generally usable
    DosVariableDescriptor* register_used[4] { };

    std::list<DosVariableDescriptor>::iterator it = variables.begin();

    while (it != variables.end()) {
        if (it->reg != CpuRegister::None && (!it->symbol->parent || (it->symbol->parent && strcmp(it->symbol->parent, parent->name) == 0))) {
            register_used[(int32_t)it->reg] = &(*it);
        }

        ++it;
    }

    for (int32_t i = 0; i < 4; i++) {
        if (suppressed_registers.find((CpuRegister)i) != suppressed_registers.end()) {
            // Skip suppressed registers
            continue;
        }

        if (!register_used[i]) {
            // Register is empty (it was not used yet in this scope)
            return (CpuRegister)i;
        }
    }

    // No unused register found
    return CpuRegister::None;
}

DosVariableDescriptor* DosExeEmitter::FindVariableByName(char* name)
{
    // Search in function-local variables
    {
        std::list<DosVariableDescriptor>::iterator it = variables.begin();

        while (it != variables.end()) {
            if (it->symbol->parent && strcmp(it->symbol->parent, parent->name) == 0 && strcmp(it->symbol->name, name) == 0) {
                return &(*it);
            }

            ++it;
        }
    }

    // Search in static (global) variables
    {
        std::list<DosVariableDescriptor>::iterator it = variables.begin();

        while (it != variables.end()) {
            if (!it->symbol->parent && strcmp(it->symbol->name, name) == 0) {
                return &(*it);
            }

            ++it;
        }
    }

    // Variable cannot be found
    ThrowOnUnreachableCode();
}

InstructionEntry* DosExeEmitter::FindNextVariableReference(DosVariableDescriptor* var, SaveReason reason)
{
    InstructionEntry* current = current_instruction;
    int32_t ip = ip_src;

    if (reason == SaveReason::Force) {
        // It was referenced in the current instruction,
        // but it doesn't matter anyway
        return current;
    }

    if (reason == SaveReason::Inside && current) {
        // Skip the current instruction,
        // start searching from the following instruction
        current = current->next;
        ip++;
    }

    while (current && ip <= parent_end_ip) {
        switch (current->type) {
            case InstructionType::Assign: {
                if ((current->assignment.op1.exp_type == ExpressionType::Variable &&
                     current->assignment.op1.value &&
                     strcmp(var->symbol->name, current->assignment.op1.value) == 0) ||
                    (current->assignment.op2.exp_type == ExpressionType::Variable &&
                     current->assignment.op2.value &&
                     strcmp(var->symbol->name, current->assignment.op2.value) == 0) ||
                    (current->assignment.dst_index.value &&
                     (strcmp(var->symbol->name, current->assignment.dst_value) == 0 ||
                      strcmp(var->symbol->name, current->assignment.dst_index.value) == 0))) {

                    return current;
                }
                break;
            }
            case InstructionType::If: {
                if ((current->if_statement.op1.exp_type == ExpressionType::Variable &&
                     current->if_statement.op1.value &&
                     strcmp(var->symbol->name, current->if_statement.op1.value) == 0) ||
                    (current->if_statement.op2.exp_type == ExpressionType::Variable &&
                     current->if_statement.op2.value &&
                     strcmp(var->symbol->name, current->if_statement.op2.value) == 0)) {

                    return current;
                }

                if (current->if_statement.ip < ip_src) {
                    // Program wants to jump backwards, it's unpredictible
                    if (var->symbol->is_temp) {
                        // Temp. variables will go out of scope
                        return nullptr;
                    } else {
                        return current;
                    }
                }
                break;
            }
            case InstructionType::Goto: {
                if (current->goto_statement.ip < ip_src) {
                    // Program wants to jump backwards, it's unpredictible
                    if (var->symbol->is_temp) {
                        // Temp. variables will go out of scope
                        return nullptr;
                    } else {
                        return current;
                    }
                }
                break;
            }
            case InstructionType::GotoLabel: {
                std::list<DosLabel>::iterator it = labels.begin();

                while (it != labels.end()) {
                    if (strcmp(it->name, current->goto_label_statement.label) == 0) {
                        // Program wants to jump backwards (to already defined label), it's unpredictible
                        if (var->symbol->is_temp) {
                            // Temp. variables will go out of scope
                            return nullptr;
                        } else {
                            return current;
                        }
                    }

                    ++it;
                }
                break;
            }
            case InstructionType::Push: {
                if (current->push_statement.symbol->exp_type == ExpressionType::Variable &&
                    current->push_statement.symbol->name &&
                    strcmp(var->symbol->name, current->push_statement.symbol->name) == 0) {

                    return current;
                }
                break;
            }
            case InstructionType::Return: {
                if (current->return_statement.op.exp_type == ExpressionType::Variable &&
                    current->return_statement.op.value &&
                    strcmp(var->symbol->name, current->return_statement.op.value) == 0) {

                    return current;
                }
                break;
            }
        }

        current = current->next;
        ip++;
    }

    return nullptr;
}

void DosExeEmitter::RefreshParentEndIp(SymbolTableEntry* symbol_table)
{
    InstructionEntry* current = current_instruction->next;
    uint32_t ip = ip_src;

    while (current) {
        SymbolTableEntry* symbol = symbol_table;

        while (symbol) {
            if (symbol->ip == ip + 1 && (symbol->type.base == BaseSymbolType::EntryPoint || symbol->type.base == BaseSymbolType::Function)) {
                parent_end_ip = ip;
                return;
            }

            symbol = symbol->next;
        }

        current = current->next;
        ip++;
    }

    parent_end_ip = ip;
}

void DosExeEmitter::SaveVariable(DosVariableDescriptor* var, SaveReason reason)
{
    if (var->symbol->size > 0) {
        // Variable is defined as pre-allocated memory, only indexed access is allowed
        ThrowOnUnreachableCode();
    }

    if (!var->is_dirty) {
        return;
    }

    int32_t var_size = compiler->GetSymbolTypeSize(var->symbol->type);

    if (var->symbol->parent) {
        if (!var->force_save && !FindNextVariableReference(var, reason)) {
            // Variable is not needed anymore, drop it...
#if _DEBUG
            Log::Write(LogType::Info, "Variable \"%s\" was optimized out", var->symbol->name);
#endif
            return;
        }

        switch (var_size) {
            case 1: {
                // Register to stack copy (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(2 + 1);
                a[0] = 0x88;   // mov rm8, r8
                a[1] = ToXrm(1, var->reg, 6);

                BackpatchLocal(a + 2, var);
                break;
            }
            case 2: {
                // Register to stack copy (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(2 + 1);
                a[0] = 0x89;   // mov rm16/32, r16/32
                a[1] = ToXrm(1, var->reg, 6);

                BackpatchLocal(a + 2, var);
                break;
            }
            case 4: {
                // Register to stack copy (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(3 + 1);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0x89;   // mov rm16/32, r16/32
                a[2] = ToXrm(1, var->reg, 6);

                BackpatchLocal(a + 3, var);
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    } else {
        switch (var_size) {
            case 1: {
                // Register to static copy
                uint8_t* a = AllocateBufferForInstruction(2 + 2);
                a[0] = 0x88;   // mov rm8, r8
                a[1] = ToXrm(0, var->reg, 6);

                BackpatchStatic(a + 2, var);
                break;
            }
            case 2: {
                // Register to static copy
                uint8_t* a = AllocateBufferForInstruction(2 + 2);
                a[0] = 0x89;   // mov rm16/32, r16/32
                a[1] = ToXrm(0, var->reg, 6);

                BackpatchStatic(a + 2, var);
                break;
            }
            case 4: {
                // Register to static copy
                uint8_t* a = AllocateBufferForInstruction(3 + 2);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0x89;   // mov rm16/32, r16/32
                a[2] = ToXrm(0, var->reg, 6);

                BackpatchStatic(a + 3, var);
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    }

    var->is_dirty = false;
}

void DosExeEmitter::SaveIndexedVariable(DosVariableDescriptor* var, InstructionOperandIndex& index, CpuRegister reg_dst)
{
    if (var->symbol->type.pointer == 0) {
        // Variable is not defined as pointer, so it cannot be indexed
        ThrowOnUnreachableCode();
    }

    SymbolType resolved_type = var->symbol->type;
    resolved_type.pointer--;
    int32_t resolved_size = compiler->GetSymbolTypeSize(resolved_type);

    switch (index.exp_type) {
        case ExpressionType::Constant: {
            int32_t value = atoi(index.value) * resolved_size;
            LoadConstantToRegister(value, CpuRegister::DI, 2);
            break;
        }
        case ExpressionType::Variable: {
            DosVariableDescriptor* index_desc = FindVariableByName(index.value);
            CopyVariableToRegister(index_desc, CpuRegister::DI, 2);

            // Multiply by size
            uint8_t shift = compiler->SizeToShift(resolved_size);
            if (shift > 0) {
                uint8_t* a = AllocateBufferForInstruction(2 + 1);
                a[0] = 0xC1;    // shl rm16, imm8
                a[1] = ToXrm(3, 4, CpuRegister::DI);
                a[2] = shift;
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    if (var->symbol->size == 0) {
        // Only pointer - it's stored somewhere else
        if (var->reg != CpuRegister::None) {
            // Pointer is already loaded in register
            uint8_t* a = AllocateBufferForInstruction(2);
            a[0] = 0x03;    // add r16, rm16
            a[1] = ToXrm(3, CpuRegister::DI, var->reg);
        } else if (!var->symbol->parent) {
            // Pointer is in static (16-bit range)
            uint8_t* a = AllocateBufferForInstruction(2 + 2);
            a[0] = 0x03;   // add r16, rm16
            a[1] = ToXrm(0, CpuRegister::DI, 6);

            BackpatchStatic(a + 2, var);
        } else {
            // Pointer is in stack (8-bit range)
            uint8_t* a = AllocateBufferForInstruction(2 + 1);
            a[0] = 0x03;   // add r16, rm16
            a[1] = ToXrm(1, CpuRegister::DI, 6);

            BackpatchLocal(a + 2, var);
        }
    }

    switch (resolved_size) {
        case 1: {
            if (var->symbol->size == 0) {
                // Register to pointer (16-bit)
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x88;   // mov rm8, r8
                a[1] = ToXrm(0, reg_dst, 5);
            } else if (!var->symbol->parent) {
                // Register to static (16-bit range)
                uint8_t* a = AllocateBufferForInstruction(2 + 2);
                a[0] = 0x88;   // mov rm8, r8
                a[1] = ToXrm(2, reg_dst, 5);

                BackpatchStatic(a + 2, var);
            } else {
                // Register to stack (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(2 + 1);
                a[0] = 0x88;   // mov rm8, r8
                a[1] = ToXrm(1, reg_dst, 3);

                BackpatchLocal(a + 2, var);
            }
            break;
        }
        case 2: {
            if (var->symbol->size == 0) {
                // Register to pointer (16-bit)
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x89;   // mov rm16, r16
                a[1] = ToXrm(0, reg_dst, 5);
            } else if (!var->symbol->parent) {
                // Register to static (16-bit range)
                uint8_t* a = AllocateBufferForInstruction(2 + 2);
                a[0] = 0x89;   // mov rm16, r16
                a[1] = ToXrm(2, reg_dst, 5);

                BackpatchStatic(a + 2, var);
            } else {
                // Register to stack (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(2 + 1);
                a[0] = 0x89;   // mov rm16, r16
                a[1] = ToXrm(1, reg_dst, 3);

                BackpatchLocal(a + 2, var);
            }
            break;
        }
        case 4: {
            if (var->symbol->size == 0) {
                // Register to pointer (16-bit)
                uint8_t* a = AllocateBufferForInstruction(3);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0x89;   // mov rm32, r32
                a[2] = ToXrm(0, reg_dst, 5);
            } else if (!var->symbol->parent) {
                // Register to static (16-bit range)
                uint8_t* a = AllocateBufferForInstruction(3 + 2);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0x89;   // mov rm32, r32
                a[2] = ToXrm(2, reg_dst, 5);

                BackpatchStatic(a + 3, var);
            } else {
                // Register to stack (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(3 + 1);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0x89;   // mov rm32, r32
                a[2] = ToXrm(1, reg_dst, 3);

                BackpatchLocal(a + 3, var);
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }
}

void DosExeEmitter::SaveAndUnloadRegister(CpuRegister reg, SaveReason reason)
{
    std::list<DosVariableDescriptor>::iterator it = variables.begin();

    while (it != variables.end()) {
        if (it->reg == reg && (!it->symbol->parent || (it->symbol->parent && strcmp(it->symbol->parent, parent->name) == 0))) {
            SaveVariable(&(*it), reason);
            it->reg = CpuRegister::None;
            break;
        }

        ++it;
    }
}

void DosExeEmitter::SaveAndUnloadAllRegisters(SaveReason reason)
{
    std::list<DosVariableDescriptor>::iterator it = variables.begin();

    while (it != variables.end()) {
        if (it->reg != CpuRegister::None && (!it->symbol->parent || (it->symbol->parent && strcmp(it->symbol->parent, parent->name) == 0))) {
            SaveVariable(&(*it), reason);
            it->reg = CpuRegister::None;
        }

        ++it;
    }
}

void DosExeEmitter::MarkRegisterAsDiscarded(CpuRegister reg)
{
    if (!parent) {
        return;
    }

    std::list<DosVariableDescriptor>::iterator it = variables.begin();

    while (it != variables.end()) {
        if (it->reg == reg && (!it->symbol->parent || (it->symbol->parent && strcmp(it->symbol->parent, parent->name) == 0))) {
            if (it->is_dirty) {
                // This should not happen, register owned by variable is discarded,
                // but variable was not written back to stack yet
                ThrowOnUnreachableCode();
            }

            it->reg = CpuRegister::None;
            break;
        }

        ++it;
    }
}

void DosExeEmitter::PushVariableToStack(DosVariableDescriptor* var, int32_t param_size)
{
    int32_t var_size = compiler->GetSymbolTypeSize(var->symbol->type);

    if (var_size < param_size) {
        // Variable expansion is needed
        CpuRegister reg = LoadVariableUnreferenced(var, param_size);

        switch (param_size) {
            case 2: {
                // Push register to parameter stack
                uint8_t* a = AllocateBufferForInstruction(1);
                a[0] = ToOpR(0x50, reg);    // push r16
                break;
            }
            case 4: {
                // Push register to parameter stack
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x66;                // Operand size prefix
                a[1] = ToOpR(0x50, reg);    // push r32
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    } else if (var->reg != CpuRegister::None) {
        // Variable is already in register
        switch (param_size) {
            case 1: {
                // Zero high part of register and push it to parameter stack
                uint8_t* a = AllocateBufferForInstruction(2 + 1);
                a[0] = 0x32;                    // xor r8, rm8
                a[1] = ToXrm(3, (uint8_t)var->reg + 4, (uint8_t)var->reg + 4);
                a[2] = ToOpR(0x50, var->reg);   // push r16
                break;
            }
            case 2: {
                // Push register to parameter stack
                uint8_t* a = AllocateBufferForInstruction(1);
                a[0] = ToOpR(0x50, var->reg);   // push r16
                break;
            }
            case 4: {
                // Push register to parameter stack
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x66;                    // Operand size prefix
                a[1] = ToOpR(0x50, var->reg);   // push r32
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    } else {
        // Variable is in memory
        switch (param_size) {
            case 1: {
                CpuRegister reg_temp = GetUnusedRegister();

                if (!var->symbol->parent) {
                    // Static push using register (16-bit range)
                    uint8_t* a1 = AllocateBufferForInstruction(3 + 2);
                    a1[0] = 0x0F;
                    a1[1] = 0xB6;   // movzx r16, rm8 (i386+)
                    a1[2] = ToXrm(0, reg_temp, 6);

                    BackpatchStatic(a1 + 3, var);
                } else {
                    // Stack push using register (8-bit range)
                    uint8_t* a1 = AllocateBufferForInstruction(3 + 1);
                    a1[0] = 0x0F;
                    a1[1] = 0xB6;   // movzx r16, rm8 (i386+)
                    a1[2] = ToXrm(1, reg_temp, 6);

                    BackpatchLocal(a1 + 3, var);
                }

                uint8_t* a2 = AllocateBufferForInstruction(1);
                a2[0] = ToOpR(0x50, reg_temp);  // push r16
                break;
            }
            case 2: {
                if (!var->symbol->parent) {
                    // Static push (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 2);
                    a[0] = 0xFF;                // push rm16
                    a[1] = ToXrm(0, 6, 6);

                    BackpatchStatic(a + 2, var);
                } else {
                    // Stack push (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 1);
                    a[0] = 0xFF;                // push rm16
                    a[1] = ToXrm(1, 6, 6);

                    BackpatchLocal(a + 2, var);
                }
                break;
            }
            case 4: {
                if (!var->symbol->parent) {
                    // Static push (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(3 + 2);
                    a[0] = 0x66;                // Operand size prefix
                    a[1] = 0xFF;                // push rm32
                    a[2] = ToXrm(0, 6, 6);

                    BackpatchStatic(a + 3, var);
                } else {
                    // Stack push (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(3 + 1);
                    a[0] = 0x66;                // Operand size prefix
                    a[1] = 0xFF;                // push rm32
                    a[2] = ToXrm(1, 6, 6);

                    BackpatchLocal(a + 3, var);
                }
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    }
}

CpuRegister DosExeEmitter::LoadVariableUnreferenced(DosVariableDescriptor* var, int32_t desired_size)
{
    if (var->symbol->size > 0) {

        if (desired_size != 2) {
            ThrowOnUnreachableCode();
        }

        return LoadVariablePointer(var, true);
    }

    int32_t var_size = compiler->GetSymbolTypeSize(var->symbol->type);

    CpuRegister reg_dst;
    if (var->reg == CpuRegister::None) {
        // Not loaded in any register yet
        reg_dst = GetUnusedRegister();
    } else {
        reg_dst = var->reg;

        if (var_size < desired_size) {
            // Expansion is needed
            CpuRegister unused = TryGetUnusedRegister();
            if (unused != CpuRegister::None) {
                // Unused register found, it will be used for faster expansion
                reg_dst = unused;
            }
        }
    }

    CopyVariableToRegister(var, reg_dst, desired_size);
    return reg_dst;
}

CpuRegister DosExeEmitter::LoadVariablePointer(DosVariableDescriptor* var, bool force_reference)
{
    CpuRegister reg_dst = GetUnusedRegister();

    // Hardcoded 16-bit pointer size
    if (!force_reference && var->symbol->size == 0) { // It's already pointer
        return LoadVariableUnreferenced(var, 2);
    }
    
    if (var->symbol->parent) { // Local (stack)
        uint8_t* a = AllocateBufferForInstruction(2 + 1);
        a[0] = 0x8D;    // lea r16, m
        a[1] = ToXrm(1, reg_dst, 6);

        BackpatchLocal(a + 2, var);
    } else { // Static
        uint8_t* a = AllocateBufferForInstruction(1 + 2);
        a[0] = ToOpR(0xB8, reg_dst);    // mov r16, imm16

        BackpatchStatic(a + 1, var);
    }

    return reg_dst;
}

CpuRegister DosExeEmitter::LoadIndexedVariable(DosVariableDescriptor* var, InstructionOperandIndex& index, int32_t desired_size)
{
    if (var->symbol->type.pointer == 0) {
        ThrowOnUnreachableCode();
    }

    SymbolType resolved_type = var->symbol->type;
    resolved_type.pointer--;
    int32_t resolved_size = compiler->GetSymbolTypeSize(resolved_type);

    switch (index.exp_type) {
        case ExpressionType::Constant: {
            int32_t value = atoi(index.value) * resolved_size;
            LoadConstantToRegister(value, CpuRegister::SI, 2);
            break;
        }
        case ExpressionType::Variable: {
            DosVariableDescriptor* index_desc = FindVariableByName(index.value);
            CopyVariableToRegister(index_desc, CpuRegister::SI, 2);

            // Multiply by size
            uint8_t shift = compiler->SizeToShift(resolved_size);
            if (shift > 0) {
                uint8_t* a = AllocateBufferForInstruction(2 + 1);
                a[0] = 0xC1;    // shl rm16, imm8
                a[1] = ToXrm(3, 4, CpuRegister::SI);
                a[2] = shift;
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    if (var->symbol->size == 0) {
        if (var->reg != CpuRegister::None) {
            // Pointer is already loaded in register
            uint8_t* a = AllocateBufferForInstruction(2);
            a[0] = 0x03;    // add r16, rm16
            a[1] = ToXrm(3, CpuRegister::SI, var->reg);
        } else if (!var->symbol->parent) {
            // Pointer is in static (16-bit range)
            uint8_t* a = AllocateBufferForInstruction(2 + 2);
            a[0] = 0x03;   // add r16, rm16
            a[1] = ToXrm(0, CpuRegister::SI, 6);

            BackpatchStatic(a + 2, var);
        } else {
            // Pointer is in stack (8-bit range)
            uint8_t* a = AllocateBufferForInstruction(2 + 1);
            a[0] = 0x03;   // add r16, rm16
            a[1] = ToXrm(1, CpuRegister::SI, 6);

            BackpatchLocal(a + 2, var);
        }
    }

    CpuRegister reg_dst = GetUnusedRegister();

    switch (resolved_size) {
        case 1: {
            if (desired_size == 4) {
                if (var->symbol->size == 0) {
                    // Pointer to register (16-bit)
                    uint8_t* a = AllocateBufferForInstruction(4);
                    a[0] = 0x66;    // Operand size prefix
                    a[1] = 0x0F;
                    a[2] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[3] = ToXrm(0, reg_dst, 4);
                } else if (!var->symbol->parent) {
                    // Static to register (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(4 + 2);
                    a[0] = 0x66;    // Operand size prefix
                    a[1] = 0x0F;
                    a[2] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[3] = ToXrm(2, reg_dst, 4);

                    BackpatchStatic(a + 4, var);
                } else {
                    // Stack to register (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(4 + 1);
                    a[0] = 0x66;    // Operand size prefix
                    a[1] = 0x0F;
                    a[2] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[3] = ToXrm(1, reg_dst, 2);

                    BackpatchLocal(a + 4, var);
                }
            } else if (desired_size == 2) {
                if (var->symbol->size == 0) {
                    // Pointer to register (16-bit)
                    uint8_t* a = AllocateBufferForInstruction(3);
                    a[0] = 0x0F;
                    a[1] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[2] = ToXrm(0, reg_dst, 4);
                } else if (!var->symbol->parent) {
                    // Static to register (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(3 + 2);
                    a[0] = 0x0F;
                    a[1] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[2] = ToXrm(2, reg_dst, 4);

                    BackpatchStatic(a + 3, var);
                } else {
                    // Stack to register (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(3 + 1);
                    a[0] = 0x0F;
                    a[1] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[2] = ToXrm(1, reg_dst, 2);

                    BackpatchLocal(a + 3, var);
                }
            } else {
                if (var->symbol->size == 0) {
                    // Pointer to register (16-bit)
                    uint8_t* a = AllocateBufferForInstruction(2);
                    a[0] = 0x8A;   // mov r8, rm8
                    a[1] = ToXrm(0, reg_dst, 4);
                } else if (!var->symbol->parent) {
                    // Static to register (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 2);
                    a[0] = 0x8A;   // mov r8, rm8
                    a[1] = ToXrm(2, reg_dst, 4);

                    BackpatchStatic(a + 2, var);
                } else {
                    // Stack to register (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 1);
                    a[0] = 0x8A;   // mov r8, rm8
                    a[1] = ToXrm(1, reg_dst, 2);

                    BackpatchLocal(a + 2, var);
                }
            }
            break;
        }
        case 2: {
            if (desired_size == 4) {
                if (var->symbol->size == 0) {
                    // Pointer to register (16-bit)
                    uint8_t* a = AllocateBufferForInstruction(3);
                    a[0] = 0x0F;
                    a[1] = 0xB7;    // movzx r32, rm16 (i386+)
                    a[2] = ToXrm(0, reg_dst, 4);
                } else if (!var->symbol->parent) {
                    // Static to register (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(3 + 2);
                    a[0] = 0x0F;
                    a[1] = 0xB7;    // movzx r32, rm16 (i386+)
                    a[2] = ToXrm(2, reg_dst, 4);

                    BackpatchStatic(a + 3, var);
                } else {
                    // Stack to register (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(3 + 1);
                    a[0] = 0x0F;
                    a[1] = 0xB7;    // movzx r32, rm16 (i386+)
                    a[2] = ToXrm(1, reg_dst, 2);

                    BackpatchLocal(a + 3, var);
                }
            } else {
                if (var->symbol->size == 0) {
                    // Pointer to register (16-bit)
                    uint8_t* a = AllocateBufferForInstruction(2);
                    a[0] = 0x8B;   // mov r16, rm16
                    a[1] = ToXrm(0, reg_dst, 4);
                } else if (!var->symbol->parent) {
                    // Static to register (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 2);
                    a[0] = 0x8B;   // mov r16, rm16
                    a[1] = ToXrm(2, reg_dst, 4);

                    BackpatchStatic(a + 2, var);
                } else {
                    // Stack to register (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 1);
                    a[0] = 0x8B;   // mov r16, rm16
                    a[1] = ToXrm(1, reg_dst, 2);

                    BackpatchLocal(a + 2, var);
                }
            }
            break;
        }
        case 4: {
            if (var->symbol->size == 0) {
                // Pointer to register (16-bit)
                uint8_t* a = AllocateBufferForInstruction(3);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0x8B;   // mov r32, rm32
                a[2] = ToXrm(0, reg_dst, 4);
            } else if (!var->symbol->parent) {
                // Static to register (16-bit range)
                uint8_t* a = AllocateBufferForInstruction(3 + 2);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0x8B;   // mov r32, rm32
                a[2] = ToXrm(2, reg_dst, 4);

                BackpatchStatic(a + 3, var);
            } else {
                // Stack to register copy (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(3 + 1);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0x8B;   // mov r32, rm32
                a[2] = ToXrm(1, reg_dst, 2);

                BackpatchLocal(a + 3, var);
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    return reg_dst;
}

void DosExeEmitter::CopyVariableToRegister(DosVariableDescriptor* var, CpuRegister reg_dst, int32_t desired_size)
{
    if (var->symbol->size > 0) {
        ThrowOnUnreachableCode();
    }

    int32_t var_size = compiler->GetSymbolTypeSize(var->symbol->type);

    if (var->reg != CpuRegister::None) {
        if (var->reg == reg_dst && var_size >= desired_size) {
            // Variable is already in desired register with desired size
            SaveVariable(var, SaveReason::Inside);
            var->reg = CpuRegister::None;
            return;
        }

        CpuRegister reg_src = var->reg;

        if (var->reg == reg_dst) {
            // Variable is in desired register, remove ownership
            SaveVariable(var, SaveReason::Inside);
            var->reg = CpuRegister::None;
        } else {
            // Variable is in another register
            SaveAndUnloadRegister(reg_dst, SaveReason::Inside);
        }

        // Copy value to desired register
        switch (var_size) {
            case 1: {
                if (desired_size == 4) {
                    uint8_t* a = AllocateBufferForInstruction(4);
                    a[0] = 0x66;    // Operand size prefix
                    a[1] = 0x0F;
                    a[2] = 0xB6;    // movzx r32, rm8 (i386+)
                    a[3] = ToXrm(3, reg_dst, reg_src);
                } else if (desired_size == 2) {
                    uint8_t* a = AllocateBufferForInstruction(3);
                    a[0] = 0x0F;
                    a[1] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[2] = ToXrm(3, reg_dst, reg_src);
                } else {
                    uint8_t* a = AllocateBufferForInstruction(2);
                    a[0] = 0x8A;    // mov r8, rm8
                    a[1] = ToXrm(3, reg_dst, reg_src);
                }
                break;
            }
            case 2: {
                if (desired_size == 4) {
                    uint8_t* a = AllocateBufferForInstruction(4);
                    a[0] = 0x66;    // Operand size prefix
                    a[1] = 0x0F;
                    a[2] = 0xB7;    // movzx r32, rm16 (i386+)
                    a[3] = ToXrm(3, reg_dst, reg_src);
                } else {
                    uint8_t* a = AllocateBufferForInstruction(2);
                    a[0] = 0x8B;   // mov r16, rm16
                    a[1] = ToXrm(3, reg_dst, reg_src);
                }
                break;
            }
            case 4: {
                uint8_t* a = AllocateBufferForInstruction(3);
                a[0] = 0x66;        // Operand size prefix
                a[1] = 0x8B;        // mov r32, rm32
                a[2] = ToXrm(3, reg_dst, reg_src);
                break;
            }

            default: ThrowOnUnreachableCode();
        }
        return;
    }

    SaveAndUnloadRegister(reg_dst, SaveReason::Inside);

    switch (var_size) {
        case 1: {
            if (desired_size == 4) {
                if (!var->symbol->parent) {
                    // Static to register copy (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(4 + 2);
                    a[0] = 0x66;    // Operand size prefix
                    a[1] = 0x0F;
                    a[2] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[3] = ToXrm(0, reg_dst, 6);

                    BackpatchStatic(a + 4, var);
                } else {
                    // Stack to register copy (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(4 + 1);
                    a[0] = 0x66;    // Operand size prefix
                    a[1] = 0x0F;
                    a[2] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[3] = ToXrm(1, reg_dst, 6);

                    BackpatchLocal(a + 4, var);
                }
            } else if (desired_size == 2) {
                if (!var->symbol->parent) {
                    // Static to register copy (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(3 + 2);
                    a[0] = 0x0F;
                    a[1] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[2] = ToXrm(0, reg_dst, 6);

                    BackpatchStatic(a + 3, var);
                } else {
                    // Stack to register copy (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(3 + 1);
                    a[0] = 0x0F;
                    a[1] = 0xB6;    // movzx r16, rm8 (i386+)
                    a[2] = ToXrm(1, reg_dst, 6);

                    BackpatchLocal(a + 3, var);
                }
            } else {
                if (!var->symbol->parent) {
                    // Static to register copy (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 2);
                    a[0] = 0x8A;   // mov r8, rm8
                    a[1] = ToXrm(0, reg_dst, 6);

                    BackpatchStatic(a + 2, var);
                } else {
                    // Stack to register copy (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 1);
                    a[0] = 0x8A;   // mov r8, rm8
                    a[1] = ToXrm(1, reg_dst, 6);

                    BackpatchLocal(a + 2, var);
                }
            }
            break;
        }
        case 2: {
            if (desired_size == 4) {
                if (!var->symbol->parent) {
                    // Static to register copy (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(4 + 2);
                    a[0] = 0x66;   // Operand size prefix
                    a[1] = 0x0F;
                    a[2] = 0xB7;    // movzx r32, rm16 (i386+)
                    a[3] = ToXrm(0, reg_dst, 6);

                    BackpatchStatic(a + 4, var);
                } else {
                    // Stack to register copy (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(4 + 1);
                    a[0] = 0x66;   // Operand size prefix
                    a[1] = 0x0F;
                    a[2] = 0xB7;    // movzx r32, rm16 (i386+)
                    a[3] = ToXrm(1, reg_dst, 6);

                    BackpatchLocal(a + 4, var);
                }
            } else {
                if (!var->symbol->parent) {
                    // Static to register copy (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 2);
                    a[0] = 0x8B;   // mov r16, rm16
                    a[1] = ToXrm(0, reg_dst, 6);

                    BackpatchStatic(a + 2, var);
                } else {
                    // Stack to register copy (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 1);
                    a[0] = 0x8B;   // mov r16, rm16
                    a[1] = ToXrm(1, reg_dst, 6);

                    BackpatchLocal(a + 2, var);
                }
            }
            break;
        }
        case 4: {
            if (!var->symbol->parent) {
                // Static to register copy (16-bit range)
                uint8_t* a = AllocateBufferForInstruction(3 + 2);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0x8B;   // mov r32, rm32
                a[2] = ToXrm(0, reg_dst, 6);

                BackpatchStatic(a + 3, var);
            } else {
                // Stack to register copy (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(3 + 1);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0x8B;   // mov r32, rm32
                a[2] = ToXrm(1, reg_dst, 6);

                BackpatchLocal(a + 3, var);
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }
}

void DosExeEmitter::LoadConstantToRegister(int32_t value, CpuRegister reg)
{
    MarkRegisterAsDiscarded(reg);

    if (value == (int8_t)value || value == (uint8_t)value) {
        uint8_t* a = AllocateBufferForInstruction(1 + 1);
        a[0] = ToOpR(0xB0, reg);    // mov r8, imm8
        *(uint8_t*)(a + 1) = (int8_t)value;
    } else if (value == (int16_t)value || value == (uint16_t)value) {
        uint8_t* a = AllocateBufferForInstruction(1 + 2);
        a[0] = ToOpR(0xB8, reg);     // mov r16, imm16
        *(uint16_t*)(a + 1) = (int16_t)value;
    } else {
        uint8_t* a = AllocateBufferForInstruction(2 + 4);
        a[0] = 0x66;                // Operand size prefix
        a[1] = ToOpR(0xB8, reg);    // mov r32, imm32
        *(uint32_t*)(a + 2) = value;
    }
}

void DosExeEmitter::LoadConstantToRegister(int32_t value, CpuRegister reg, int32_t desired_size)
{
    if (value == 0) {
        // If the value is zero, xor is faster
        ZeroRegister(reg, desired_size);
        return;
    }

    MarkRegisterAsDiscarded(reg);

    switch (desired_size) {
        case 1: {
            uint8_t* a = AllocateBufferForInstruction(1 + 1);
            a[0] = ToOpR(0xB0, reg);    // mov r8, imm8
            *(uint8_t*)(a + 1) = (int8_t)value;
            break;
        }
        case 2: {
            uint8_t* a = AllocateBufferForInstruction(1 + 2);
            a[0] = ToOpR(0xB8, reg);    // mov r16, imm16
            *(uint16_t*)(a + 1) = (int16_t)value;
            break;
        }
        case 4:
        case 8: {
            uint8_t* a = AllocateBufferForInstruction(2 + 4);
            a[0] = 0x66;                // Operand size prefix
            a[1] = ToOpR(0xB8, reg);    // mov r32, imm32
            *(uint32_t*)(a + 2) = value;
            break;
        }

        default: ThrowOnUnreachableCode();
    }
}

void DosExeEmitter::ZeroRegister(CpuRegister reg, int32_t desired_size)
{
    MarkRegisterAsDiscarded(reg);

    switch (desired_size) {
        case 1: {
            uint8_t* a = AllocateBufferForInstruction(2);
            a[0] = 0x32;   // xor r8, rm8
            a[1] = ToXrm(3, reg, reg);
            break;
        }
        case 2: {
            uint8_t* a = AllocateBufferForInstruction(2);
            a[0] = 0x33;   // xor r16, rm16
            a[1] = ToXrm(3, reg, reg);
            break;
        }
        case 4:
        case 8: {
            uint8_t* a = AllocateBufferForInstruction(3);
            a[0] = 0x66;   // Operand size prefix
            a[1] = 0x33;   // xor r32, rm32
            a[2] = ToXrm(3, reg, reg);
            break;
        }

        default: ThrowOnUnreachableCode();
    }
}

void DosExeEmitter::BackpatchAddresses()
{
    std::list<DosBackpatchInstruction>::iterator it = backpatch.begin();

    while (it != backpatch.end()) {
        if (it->target == DosBackpatchTarget::IP && it->ip_src == ip_src) {
            switch (it->type) {
                case DosBackpatchType::ToRel8: {
                    int32_t rel8 = (int32_t)(ip_src_to_dst[ip_src] - it->backpatch_ip);
                    if (rel8 < INT8_MIN || rel8 > INT8_MAX) {
                        throw CompilerException(CompilerExceptionSource::Compilation,
                            "Compiler cannot generate that high relative address");
                    }

                    *(int8_t*)(buffer + it->backpatch_offset) = (int8_t)rel8;
                    break;
                }
                case DosBackpatchType::ToRel16: {
                    int16_t rel16 = (int16_t)(ip_src_to_dst[ip_src] - it->backpatch_ip);
                    *(int16_t*)(buffer + it->backpatch_offset) = rel16;
                    break;
                }

                default: ThrowOnUnreachableCode();
            }

            it = backpatch.erase(it);
        } else {
            ++it;
        }
    }
}

void DosExeEmitter::BackpatchLabels(const DosLabel& label, DosBackpatchTarget target)
{
    std::list<DosBackpatchInstruction>::iterator it = backpatch.begin();

    while (it != backpatch.end()) {
        if (it->target == target && strcmp(it->value, label.name) == 0) {
            switch (it->type) {
                case DosBackpatchType::ToRel8: {
                    int32_t rel8 = (int32_t)(label.ip_dst - it->backpatch_ip);
                    if (rel8 < INT8_MIN || rel8 > INT8_MAX) {
                        throw CompilerException(CompilerExceptionSource::Compilation,
                            "Compiler cannot generate that high relative address");
                    }

                    *(int8_t*)(buffer + it->backpatch_offset) = (int8_t)rel8;
                    break;
                }
                case DosBackpatchType::ToRel16: {
                    int16_t rel16 = (int16_t)(label.ip_dst - it->backpatch_ip);
                    *(int16_t*)(buffer + it->backpatch_offset) = rel16;
                    break;
                }
                case DosBackpatchType::ToDsAbs16: {
                    int16_t abs16 = (int16_t)label.ip_dst;
                    abs16 += 0x0100; // Program Segment Prefix
                    *(int16_t*)(buffer + it->backpatch_offset) = abs16;
                    break;
                }
                case DosBackpatchType::ToStack8: {
                    *(int8_t*)(buffer + it->backpatch_offset) = (int8_t)label.ip_dst;
                    break;
                }

                default: ThrowOnUnreachableCode();
            }

            it = backpatch.erase(it);
        } else {
            ++it;
        }
    }
}

void DosExeEmitter::CheckBackpatchListIsEmpty(DosBackpatchTarget target)
{
    std::list<DosBackpatchInstruction>::iterator it = backpatch.begin();

    while (it != backpatch.end()) {
        if (it->target == target) {
            if (target == DosBackpatchTarget::Function) {
                std::string message = "Function \"";
                message += it->value;
                message += "\" could not be resolved";
                throw CompilerException(CompilerExceptionSource::Statement, message);
            } else if (target == DosBackpatchTarget::String) {
                std::string message = "String \"";
                message += it->value;
                message += "\" could not be resolved";
                throw CompilerException(CompilerExceptionSource::Statement, message);
            } else {
                ThrowOnUnreachableCode();
            }
        }

        ++it;
    }
}

void DosExeEmitter::CheckReturnStatementPresent()
{
    if (parent && !was_return) {
        if (parent->return_type.base == BaseSymbolType::Void && parent->return_type.pointer == 0) {
            EmitReturn(nullptr, compiler->GetSymbols());

            // Adjust "ip_src_to_dst" mapping, because of unloaded registers
            ip_src_to_dst[ip_src] = ip_dst;
        } else {
            std::string message = "Function \"";
            message += parent->name;
            message += "\" must have \"return\" as the last statement";
            throw CompilerException(CompilerExceptionSource::Compilation, message);
        }
    }
}

void DosExeEmitter::ProcessSymbolLinkage(SymbolTableEntry* symbol_table)
{
Retry:
    // Check if any symbol is linked with current IP and do corresponding action
    SymbolTableEntry* symbol = symbol_table;

    while (symbol) {
        if (symbol->ip == ip_src) {
            if (symbol->type.base == BaseSymbolType::EntryPoint) {
                // Start of entry point
                EmitFunctionEpilogue();

                EmitEntryPointPrologue(symbol);

                RefreshParentEndIp(symbol_table);

                Log::PopIndent();
                Log::Write(LogType::Info, "Compiling entry point...");
                Log::PushIndent();
            } else if (symbol->type.base == BaseSymbolType::Function) {
                // Start of standard function
                EmitFunctionEpilogue();

                if (symbol->ref_count == 0) {
                    // Function is not referenced, it will be optimized out
                    Log::PopIndent();
                    Log::Write(LogType::Info, "Function \"%s\" was optimized out", symbol->name);
                    Log::PushIndent();

                    // Find the beginning of the next function to skip unused lines
                    current_instruction = current_instruction->next;
                    ip_src++;

                    while (current_instruction) {
                        symbol = symbol_table;
                        while (symbol) {
                            if (symbol->ip == ip_src &&
                                (symbol->type.base == BaseSymbolType::Function || symbol->type.base == BaseSymbolType::EntryPoint)) {
                                goto CanContinue;
                            }

                            symbol = symbol->next;
                        }

                        current_instruction = current_instruction->next;
                        ip_src++;
                    }

                CanContinue:
                    // Adjust "ip_src_to_dst" mapping, because of unloaded registers
                    ip_src_to_dst[ip_src] = ip_dst;

                    goto Retry;
                }

                EmitFunctionPrologue(symbol, symbol_table);

                RefreshParentEndIp(symbol_table);

                Log::PopIndent();
                Log::Write(LogType::Info, "Compiling function \"%s\"...", parent->name);
                Log::PushIndent();
            } else if (symbol->type.base == BaseSymbolType::Label) {
                // Label

                // Unload all registers before label, so we can
                // jump to it without any issues
                SaveAndUnloadAllRegisters(SaveReason::Before);

                // Adjust "ip_src_to_dst" mapping, because of unloaded registers
                ip_src_to_dst[ip_src] = ip_dst;

                BackpatchLabels({ symbol->name, ip_dst }, DosBackpatchTarget::Label);
            }
        }

        symbol = symbol->next;
    }
}

void DosExeEmitter::EmitEntryPointPrologue(SymbolTableEntry* function)
{
    parent = function;

    // Prepare for startup
    AsmMov(CpuRegister::AX, CpuSegment::DS);
    AsmMov(CpuSegment::SS, CpuRegister::AX);
    AsmMov(CpuSegment::ES, CpuRegister::AX);

    // Create new call frame
    uint8_t* l4 = AllocateBufferForInstruction(3);
    l4[0] = 0x66;    // Operand size prefix
    l4[1] = 0x8B;    // mov r32 (ebp), rm32 (esp)
    l4[2] = ToXrm(3, CpuRegister::BP, CpuRegister::SP);

    // Allocate space for local variables
    uint8_t* l5 = AllocateBufferForInstruction(2 + 2);
    l5[0] = 0x81;    // sub rm32 (esp), imm32 <size>
    l5[1] = ToXrm(3, 5, CpuRegister::SP);

    parent_stack_offset = (uint32_t)((l5 + 2) - buffer);

    // Clear function-local labels
    labels.clear();
}

void DosExeEmitter::EmitFunctionPrologue(SymbolTableEntry* function, SymbolTableEntry* symbol_table)
{
    parent = function;

    // Create backpatch information
    BackpatchLabels({ function->name, ip_dst }, DosBackpatchTarget::Function);

    functions.push_back({ function->name, ip_dst });

    // Create new call frame
    AsmProcEnter();

    // Allocate space for local variables in stack
    int32_t stack_param_size = 0;

    std::list<DosVariableDescriptor>::iterator it = variables.begin();

    while (it != variables.end()) {
        if (it->symbol->parent && strcmp(it->symbol->parent, parent->name) == 0) {
            if (it->symbol->parameter) { // Parameter
                int32_t size = compiler->GetSymbolTypeSize(it->symbol->type);
                if (size < 2) { // Min. push size is 2 bytes
                    size = 2;
                }

                // 4 bytes are used by ebp, 2 bytes are used for return address
                it->location = stack_param_size + 6;

                stack_param_size += size;
            }
        }

        ++it;
    }

    uint8_t* a = AllocateBufferForInstruction(2 + 2);
    a[0] = 0x81;    // sub rm32 (esp), imm32 <size>
    a[1] = ToXrm(3, 5, CpuRegister::SP);

    parent_stack_offset = (uint32_t)((a + 2) - buffer);

    // Clear function-local labels
    labels.clear();
}

void DosExeEmitter::EmitFunctionEpilogue()
{
    if (!parent) {
        return;
    }

    CheckReturnStatementPresent();

    // Adjust stack for function-local variables
    int32_t stack_var_size = 0;
    int32_t stack_saved_size = 0;
    std::list<DosVariableDescriptor>::iterator it = variables.begin();

    while (it != variables.end()) {
        if (it->symbol->parent && strcmp(it->symbol->parent, parent->name) == 0) {
            if (!it->symbol->parameter) { // Local variable
                int32_t size;
                if (it->symbol->size > 0) {
                    SymbolType resolved_type = it->symbol->type;
                    resolved_type.pointer--;
                    size = it->symbol->size * compiler->GetSymbolTypeSize(resolved_type);
                } else {
                    size = compiler->GetSymbolTypeSize(it->symbol->type);
                }

                if (it->symbol->ref_count == 0) {
                    stack_saved_size += size;
                } else {
                    stack_var_size += size;

                    it->location = -stack_var_size;

                    BackpatchLabels({ it->symbol->name, it->location }, DosBackpatchTarget::Local);
                }
            }
        }

        ++it;
    }

    if (!parent_stack_offset) {
        ThrowOnUnreachableCode();
    }

    if (stack_var_size >= INT8_MAX) {
        throw CompilerException(CompilerExceptionSource::Compilation,
            "Compiler cannot generate that high address offset");
    }

    *(uint16_t*)(buffer + parent_stack_offset) = stack_var_size;

    CheckBackpatchListIsEmpty(DosBackpatchTarget::Local);

    Log::Write(LogType::Verbose, "Uses %d bytes in stack (%d bytes saved)", stack_var_size, stack_saved_size);

    // Labels are function-local too, so they must be resolved at this point
    CheckBackpatchListIsEmpty(DosBackpatchTarget::Label);

    parent = nullptr;
}

void DosExeEmitter::EmitAssign(InstructionEntry* i)
{
    switch (i->assignment.type) {
        case AssignType::None: {
            EmitAssignNone(i);
            break;
        }
        case AssignType::Negation: {
            EmitAssignNegation(i);
            break;
        }
        case AssignType::Add:
        case AssignType::Subtract: {
            EmitAssignAddSubtract(i);
            break;
        }
        case AssignType::Multiply: {
            EmitAssignMultiply(i);
            break;
        }
        case AssignType::Divide:
        case AssignType::Remainder: {
            EmitAssignDivide(i);
            break;
        }
        case AssignType::ShiftLeft:
        case AssignType::ShiftRight: {
            EmitAssignShift(i);
            break;
        }

        default: ThrowOnUnreachableCode();
    }
}

void DosExeEmitter::EmitAssignNone(InstructionEntry* i)
{
    DosVariableDescriptor* dst = FindVariableByName(i->assignment.dst_value);

    switch (i->assignment.op1.exp_type) {
        case ExpressionType::Constant: {
            CpuRegister reg_dst;
            if (i->assignment.op1.type.base == BaseSymbolType::String) {
                // Load string address to register
                reg_dst = GetUnusedRegister();

                uint8_t* a = AllocateBufferForInstruction(1 + 2);
                a[0] = ToOpR(0xB8, reg_dst);   // mov r16, imm16

                // Create backpatch info for string
                BackpatchString(a + 1, i->assignment.op1.value);
            } else {
                // ToDo: No need to allocate register for constant value
                //var->value = i->assignment.op1_value;

                // Load constant to register
                reg_dst = GetUnusedRegister();

                int32_t value = atoi(i->assignment.op1.value);

                int32_t dst_size = compiler->GetSymbolTypeSize(dst->symbol->type);
                LoadConstantToRegister(value, reg_dst, dst_size);
            }

            if (i->assignment.dst_index.value) {
                // Array values are not cached
                SaveIndexedVariable(dst, i->assignment.dst_index, reg_dst);
            } else {
                dst->reg = reg_dst;
                dst->is_dirty = true;
            }
            dst->last_used = ip_src;
            break;
        }
        case ExpressionType::Variable: {
            DosVariableDescriptor* op1 = FindVariableByName(i->assignment.op1.value);

            int32_t op1_size = compiler->GetSymbolTypeSize(op1->symbol->type);
            int32_t dst_size = compiler->GetSymbolTypeSize(dst->symbol->type);

            CpuRegister reg_dst;
            if (op1->symbol->exp_type == ExpressionType::Constant) {
                reg_dst = GetUnusedRegister();

                if (op1->symbol->type.base == BaseSymbolType::String) {
                    ThrowOnUnreachableCode();
                    /*
                    // Load string address to register
                    reg_dst = GetUnusedRegister();

                    uint8_t* a = AllocateBufferForInstruction(1 + 2);
                    a[0] = ToOpR(0xB8, reg_dst);    // mov r16, imm16

                    // Create backpatch info for string
                    {
                        DosBackpatchInstruction b { };
                        b.target = DosBackpatchTarget::String;
                        b.type = DosBackpatchType::ToDsAbs16;
                        b.backpatch_offset = (a + 1) - buffer;
                        b.value = i->assignment.op1.value;
                        backpatch.push_back(b);
                    }
                    */
                } else {
                    int32_t value = atoi(op1->value);
                    LoadConstantToRegister(value, reg_dst, dst_size);
                }
            } else if (i->assignment.op1.index.value) {
                reg_dst = LoadIndexedVariable(op1, i->assignment.op1.index, dst_size);
            } else {
                bool needs_reference = (!i->assignment.dst_index.value && dst->symbol->type.pointer > op1->symbol->type.pointer);
                if (needs_reference) {
                    // Reference to variable
                    op1->force_save = true;

                    reg_dst = LoadVariablePointer(op1, true);
                } else {
                    reg_dst = LoadVariableUnreferenced(op1, dst_size);
                }
            }

            if (i->assignment.dst_index.value) {
                // Array values are not cached
                SaveIndexedVariable(dst, i->assignment.dst_index, reg_dst);
            } else {
                dst->reg = reg_dst;
                dst->is_dirty = true;
            }
            dst->last_used = ip_src;
            break;
        }

        default: ThrowOnUnreachableCode();
    }
}

void DosExeEmitter::EmitAssignNegation(InstructionEntry* i)
{
    DosVariableDescriptor* dst = FindVariableByName(i->assignment.dst_value);

    // ToDo: Compile-time evaluation
    /*if (dst->symbol->exp_type == ExpressionType::Constant) {
        size_t str_length = strlen(i->assignment.op1.value);
        if (i->assignment.op1.value[0] == '-') {
            char* value_neg = new char[str_length];
            memcpy(value_neg, i->assignment.op1.value + 1, str_length - 1);
            value_neg[str_length - 1] = '\0';
            dst->value = value_neg;
        } else {
            char* value_neg = new char[str_length + 2];
            value_neg[0] = '-';
            memcpy(value_neg + 1, i->assignment.op1.value, str_length);
            value_neg[str_length + 1] = '\0';
            dst->value = value_neg;
        }
        return;
    }*/

    CpuRegister reg_dst = dst->reg;
    if (reg_dst == CpuRegister::None) {
        reg_dst = GetUnusedRegister();
    }

    int32_t dst_size = compiler->GetSymbolTypeSize(dst->symbol->type);

    switch (i->assignment.op1.exp_type) {
        case ExpressionType::Constant: {
            int32_t value = atoi(i->assignment.op1.value);
            LoadConstantToRegister(value, reg_dst, dst_size);
            break;
        }
        case ExpressionType::Variable: {
            DosVariableDescriptor* op1 = FindVariableByName(i->assignment.op1.value);
            CopyVariableToRegister(op1, reg_dst, dst_size);
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    switch (dst_size) {
        case 1: {
            uint8_t* a = AllocateBufferForInstruction(2);
            a[0] = 0xF6;   // neg rm8
            a[1] = ToXrm(3, 3, reg_dst);
            break;
        }
        case 2: {
            uint8_t* a = AllocateBufferForInstruction(2);
            a[0] = 0xF7;   // neg rm16
            a[1] = ToXrm(3, 3, reg_dst);
            break;
        }
        case 4: {
            uint8_t* a = AllocateBufferForInstruction(3);
            a[0] = 0x66;   // Operand size prefix
            a[1] = 0xF7;   // neg rm32
            a[2] = ToXrm(1, 3, reg_dst);
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    dst->reg = reg_dst;
    dst->is_dirty = true;
    dst->last_used = ip_src;
}

void DosExeEmitter::EmitAssignAddSubtract(InstructionEntry* i)
{
    DosVariableDescriptor* dst = FindVariableByName(i->assignment.dst_value);

    if (i->assignment.type == AssignType::Add && dst->symbol->type.base == BaseSymbolType::String) {
        if (i->assignment.op1.exp_type == ExpressionType::Constant && i->assignment.op2.exp_type == ExpressionType::Constant) {
            size_t length1 = strlen(i->assignment.op1.value);
            size_t length2 = strlen(i->assignment.op2.value);

            char* concat = new char[length1 + length2 + 1];
            memcpy(concat, i->assignment.op1.value, length1);
            memcpy(concat + length1, i->assignment.op2.value, length2);
            concat[length1 + length2] = '\0';

            strings.insert(concat);

            dst->value = concat;
            //dst->symbol->exp_type = ExpressionType::Constant;

            // Load string address to register
            dst->reg = GetUnusedRegister();

            uint8_t* a = AllocateBufferForInstruction(1 + 2);
            a[0] = ToOpR(0xB8, dst->reg);   // mov r16, imm16

            // Create backpatch info for string
            BackpatchString(a + 1, concat);
        } else {
            ThrowOnUnreachableCode();
        }

        dst->is_dirty = true;
        dst->last_used = ip_src;
        return;
    }

    bool constant_swapped = false;

    if (i->assignment.op1.exp_type == ExpressionType::Constant) {
        // Constant has to be second operand, swap them
        std::swap(i->assignment.op1, i->assignment.op2);

        constant_swapped = true;
    }

    int32_t dst_size = compiler->GetSymbolTypeSize(dst->symbol->type);

    if (i->assignment.op1.exp_type == ExpressionType::Constant) {
        // Both operands are constants
        int32_t value1 = atoi(i->assignment.op1.value);
        int32_t value2 = atoi(i->assignment.op2.value);

        if (i->assignment.type == AssignType::Add) {
            value1 += value2;
        } else {
            value1 -= value2;
        }

        CpuRegister reg_dst = GetUnusedRegister();

        LoadConstantToRegister(value1, reg_dst, dst_size);

        dst->reg = reg_dst;
        dst->is_dirty = true;
        dst->last_used = ip_src;
        return;
    }

    DosVariableDescriptor* op1 = FindVariableByName(i->assignment.op1.value);

    CpuRegister reg_dst;
    if (dst == op1 && op1->reg != CpuRegister::None) {
        reg_dst = op1->reg;
    } else {
        reg_dst = LoadVariableUnreferenced(op1, dst_size);
    }

    switch (i->assignment.op2.exp_type) {
        case ExpressionType::Constant: {
            int32_t value = atoi(i->assignment.op2.value);
            if (i->assignment.type == AssignType::Subtract) {
                value = -value;
            }
            
            switch (dst_size) {
                case 1: {
                    uint8_t* a = AllocateBufferForInstruction(2 + 1);
                    a[0] = 0x80;        // add rm8, imm8
                    a[1] = ToXrm(3, 0, reg_dst);
                    *(int8_t*)(a + 2) = value;

                    if (i->assignment.type == AssignType::Subtract && constant_swapped) {
                        uint8_t* neg = AllocateBufferForInstruction(2);
                        neg[0] = 0xF6;  // neg rm8
                        neg[1] = ToXrm(3, 3, reg_dst);
                    }
                    break;
                }
                case 2: {
                    uint8_t* a = AllocateBufferForInstruction(2 + 2);
                    a[0] = 0x81;        // add rm16, imm16
                    a[1] = ToXrm(3, 0, reg_dst);
                    *(int16_t*)(a + 2) = value;

                    if (i->assignment.type == AssignType::Subtract && constant_swapped) {
                        uint8_t* neg = AllocateBufferForInstruction(2);
                        neg[0] = 0xF7;  // neg rm16
                        neg[1] = ToXrm(3, 3, reg_dst);
                    }
                    break;
                }
                case 4: {
                    uint8_t* a = AllocateBufferForInstruction(3 + 4);
                    a[0] = 0x66;        // Operand size prefix
                    a[1] = 0x81;        // add rm32, imm32
                    a[2] = ToXrm(3, 0, reg_dst);
                    *(int32_t*)(a + 3) = value;

                    if (i->assignment.type == AssignType::Subtract && constant_swapped) {
                        uint8_t* neg = AllocateBufferForInstruction(3);
                        neg[0] = 0x66;  // Operand size prefix
                        neg[1] = 0xF7;  // neg rm32
                        neg[2] = ToXrm(3, 3, reg_dst);
                    }

                    break;
                }

                default: ThrowOnUnreachableCode();
            }
            break;
        }
        case ExpressionType::Variable: {
            DosVariableDescriptor* op2 = FindVariableByName(i->assignment.op2.value);
            int32_t op2_size = compiler->GetSymbolTypeSize(op2->symbol->type);

            if (op2_size < dst_size) {
                SuppressRegister _(this, reg_dst);
                op2->reg = LoadVariableUnreferenced(op2, dst_size);
            }

            switch (dst_size) {
                case 1: {
                    uint8_t opcode = (i->assignment.type == AssignType::Add ? 0x02 : 0x2A);
                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(2);
                        a[0] = opcode; // add/sub r8, rm8
                        a[1] = ToXrm(3, reg_dst, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 2);
                        a[0] = opcode; // add/sub r8, rm8
                        a[1] = ToXrm(0, reg_dst, 6);

                        BackpatchStatic(a + 2, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 1);
                        a[0] = opcode; // add/sub r8, rm8
                        a[1] = ToXrm(1, reg_dst, 6);

                        BackpatchLocal(a + 2, op2);
                    }
                    break;
                }
                case 2: {
                    uint8_t opcode = (i->assignment.type == AssignType::Add ? 0x03 : 0x2B);
                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(2);
                        a[0] = opcode; // add/sub r16, rm16
                        a[1] = ToXrm(3, reg_dst, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 2);
                        a[0] = opcode; // add/sub r16, rm16
                        a[1] = ToXrm(0, reg_dst, 6);

                        BackpatchStatic(a + 2, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 1);
                        a[0] = opcode; // add/sub r16, rm16
                        a[1] = ToXrm(1, reg_dst, 6);

                        BackpatchLocal(a + 2, op2);
                    }
                    break;
                }
                case 4: {
                    uint8_t opcode = (i->assignment.type == AssignType::Add ? 0x03 : 0x2B);
                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(3);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = opcode; // add/sub r32, rm32
                        a[2] = ToXrm(3, reg_dst, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(3 + 2);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = opcode; // add/sub r32, rm32
                        a[2] = ToXrm(0, reg_dst, 6);

                        BackpatchStatic(a + 3, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(3 + 1);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = opcode; // add/sub r32, rm32
                        a[2] = ToXrm(1, reg_dst, 6);

                        BackpatchLocal(a + 3, op2);
                    }
                    break;
                }

                default: ThrowOnUnreachableCode();
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    dst->reg = reg_dst;
    dst->is_dirty = true;
    dst->last_used = ip_src;
}

void DosExeEmitter::EmitAssignMultiply(InstructionEntry* i)
{
    DosVariableDescriptor* dst = FindVariableByName(i->assignment.dst_value);

    int32_t dst_size = compiler->GetSymbolTypeSize(dst->symbol->type);

    if (i->assignment.op1.exp_type == ExpressionType::Constant) {
        // Constant has to be second operand, swap them
        std::swap(i->assignment.op1, i->assignment.op2);
    }

    if (i->assignment.op1.exp_type == ExpressionType::Constant) {
        // Both operands are constants - constant expression
        int32_t value1 = atoi(i->assignment.op1.value);
        int32_t value2 = atoi(i->assignment.op2.value);

        value1 *= value2;

        CpuRegister reg_dst = GetUnusedRegister();

        LoadConstantToRegister(value1, reg_dst, dst_size);

        dst->reg = reg_dst;
        dst->is_dirty = true;
        dst->last_used = ip_src;
        return;
    }

    DosVariableDescriptor* op1 = FindVariableByName(i->assignment.op1.value);

    switch (i->assignment.op2.exp_type) {
        case ExpressionType::Constant: {
            int32_t value = atoi(i->assignment.op2.value);

            SaveAndUnloadRegister(CpuRegister::AX, SaveReason::Inside);
            LoadConstantToRegister(value, CpuRegister::AX, dst_size);

            switch (dst_size) {
                case 1: {
                    if (op1->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(2);
                        a[0] = 0xF6;   // mul r8, rm8
                        a[1] = ToXrm(3, 4, op1->reg);
                    } else if (!op1->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 2);
                        a[0] = 0xF6;   // mul r8, rm8
                        a[1] = ToXrm(0, 4, 6);

                        BackpatchStatic(a + 2, op1);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 1);
                        a[0] = 0xF6;   // mul r8, rm8
                        a[1] = ToXrm(1, 4, 6);

                        BackpatchLocal(a + 2, op1);
                    }
                    break;
                }
                case 2: {
                    // DX register will be discarted after multiply
                    SaveAndUnloadRegister(CpuRegister::DX, SaveReason::Inside);

                    if (op1->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(2);
                        a[0] = 0xF7;   // mul r16, rm16
                        a[1] = ToXrm(3, 4, op1->reg);
                    } else if (!op1->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 2);
                        a[0] = 0xF7;   // mul r16, rm16
                        a[1] = ToXrm(0, 4, 6);

                        BackpatchStatic(a + 2, op1);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 1);
                        a[0] = 0xF7;   // mul r16, rm16
                        a[1] = ToXrm(1, 4, 6);

                        BackpatchLocal(a + 2, op1);
                    }
                    break;
                }
                case 4: {
                    // DX register will be discarted after multiply
                    SaveAndUnloadRegister(CpuRegister::DX, SaveReason::Inside);

                    if (op1->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(3);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = 0xF7;   // mul r32, rm32
                        a[2] = ToXrm(3, 4, op1->reg);
                    } else if (!op1->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(3 + 2);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = 0xF7;   // mul r32, rm32
                        a[2] = ToXrm(0, 4, 6);

                        BackpatchStatic(a + 3, op1);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(3 + 1);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = 0xF7;   // mul r32, rm32
                        a[2] = ToXrm(1, 4, 6);

                        BackpatchLocal(a + 3, op1);
                    }
                    break;
                }

                default: ThrowOnUnreachableCode();
            }
            break;
        }
        case ExpressionType::Variable: {
            DosVariableDescriptor* op2 = FindVariableByName(i->assignment.op2.value);

            if (op2->reg == CpuRegister::AX) {
                // If the second operand is already in AX, swap them
                // If not, it doesn't matter, one operand has to be in AX
                std::swap(op1, op2);
            }

            CopyVariableToRegister(op1, CpuRegister::AX, dst_size);

            // One operand is already in AX
            SuppressRegister _(this, CpuRegister::AX);

            int32_t op2_size = compiler->GetSymbolTypeSize(op2->symbol->type);
            if (op2_size < dst_size) {
                // Required size is higher than provided, unreference and expand it
                op2->reg = LoadVariableUnreferenced(op2, dst_size);
            }

            switch (dst_size) {
                case 1: {
                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(2);
                        a[0] = 0xF6;   // mul r8, rm8
                        a[1] = ToXrm(3, 4, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 2);
                        a[0] = 0xF6;   // mul r8, rm8
                        a[1] = ToXrm(0, 4, 6);

                        BackpatchStatic(a + 2, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 1);
                        a[0] = 0xF6;   // mul r8, rm8
                        a[1] = ToXrm(1, 4, 6);

                        BackpatchLocal(a + 2, op2);
                    }
                    break;
                }
                case 2: {
                    // DX register will be discarted after multiply
                    SaveAndUnloadRegister(CpuRegister::DX, SaveReason::Inside);

                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(2);
                        a[0] = 0xF7;   // mul r16, rm16
                        a[1] = ToXrm(3, 4, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 2);
                        a[0] = 0xF7;   // mul r16, rm16
                        a[1] = ToXrm(0, 4, 6);

                        BackpatchStatic(a + 2, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 1);
                        a[0] = 0xF7;   // mul r16, rm16
                        a[1] = ToXrm(1, 4, 6);

                        BackpatchLocal(a + 2, op2);
                    }
                    break;
                }
                case 4: {
                    // DX register will be discarted after multiply
                    SaveAndUnloadRegister(CpuRegister::DX, SaveReason::Inside);

                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(3);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = 0xF7;   // mul r32, rm32
                        a[2] = ToXrm(3, 4, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(3 + 2);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = 0xF7;   // mul r32, rm32
                        a[2] = ToXrm(0, 4, 6);

                        BackpatchStatic(a + 3, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(3 + 1);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = 0xF7;   // mul r32, rm32
                        a[2] = ToXrm(1, 4, 6);

                        BackpatchLocal(a + 3, op2);
                    }
                    break;
                }

                default: ThrowOnUnreachableCode();
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    dst->reg = CpuRegister::AX;
    dst->is_dirty = true;
    dst->last_used = ip_src;
}

void DosExeEmitter::EmitAssignDivide(InstructionEntry* i)
{
    DosVariableDescriptor* dst = FindVariableByName(i->assignment.dst_value);

    int32_t dst_size = compiler->GetSymbolTypeSize(dst->symbol->type);

    switch (i->assignment.op1.exp_type) {
        case ExpressionType::Constant: {
            int32_t value = atoi(i->assignment.op1.value);

            SaveAndUnloadRegister(CpuRegister::AX, SaveReason::Inside);
            // Load with higher size than destination to clear upper/high part
            // of the register, so the register will be ready for division
            LoadConstantToRegister(value, CpuRegister::AX, dst_size * 2);
            break;
        }
        case ExpressionType::Variable: {
            DosVariableDescriptor* op1 = FindVariableByName(i->assignment.op1.value);

            // Copy with higher size than destination to clear upper/high part
            // of the register, so the register will be ready for division
            CopyVariableToRegister(op1, CpuRegister::AX, dst_size * 2);
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    // One operand is already in AX and DX will be discarded
    SuppressRegister _1(this, CpuRegister::AX);
    SuppressRegister _2(this, CpuRegister::DX);

    DosVariableDescriptor* op2 = nullptr;
    CpuRegister op2_reg;
    switch (i->assignment.op2.exp_type) {
        case ExpressionType::Constant: {
            int32_t value = atoi(i->assignment.op2.value);

            op2_reg = GetUnusedRegister();
            LoadConstantToRegister(value, op2_reg, dst_size);
            break;
        }
        case ExpressionType::Variable: {
            op2 = FindVariableByName(i->assignment.op2.value);
            op2_reg = op2->reg;
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    switch (dst_size) {
        case 1: {
            if (op2_reg != CpuRegister::None) {
                // Register to register copy
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0xF6;   // div r8, rm8
                a[1] = ToXrm(3, 6, op2_reg);
            } else if (!op2->symbol->parent) {
                // Static to register copy (16-bit range)
                uint8_t* a = AllocateBufferForInstruction(2 + 2);
                a[0] = 0xF6;   // div r8, rm8
                a[1] = ToXrm(0, 6, 6);

                BackpatchStatic(a + 2, op2);
            } else {
                // Stack to register copy (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(2 + 1);
                a[0] = 0xF6;   // div r8, rm8
                a[1] = ToXrm(1, 6, 6);

                BackpatchLocal(a + 2, op2);
            }

            if (i->assignment.type == AssignType::Remainder) {
                // Copy remainder from AH to AL
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0x8A;   // mov r8, rm8
                a[1] = ToXrm(3, CpuRegister::AL, CpuRegister::AH);
            }

            ZeroRegister(CpuRegister::AH, 1);

            dst->reg = CpuRegister::AX;
            break;
        }
        case 2: {
            // DX register will be discarted after multiply
            SaveAndUnloadRegister(CpuRegister::DX, SaveReason::Inside);

            ZeroRegister(CpuRegister::DX, 2);

            if (op2_reg != CpuRegister::None) {
                // Register to register copy
                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0xF7;   // div r16, rm16
                a[1] = ToXrm(3, 6, op2_reg);
            } else if (!op2->symbol->parent) {
                // Static to register copy (16-bit range)
                uint8_t* a = AllocateBufferForInstruction(2 + 2);
                a[0] = 0xF7;   // div r16, rm16
                a[1] = ToXrm(0, 6, 6);

                BackpatchStatic(a + 2, op2);
            } else {
                // Stack to register copy (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(2 + 1);
                a[0] = 0xF7;   // div r16, rm16
                a[1] = ToXrm(1, 6, 6);

                BackpatchLocal(a + 2, op2);
            }

            dst->reg = (i->assignment.type == AssignType::Remainder ? CpuRegister::DX : CpuRegister::AX);
            break;
        }
        case 4: {
            // DX register will be discarted after multiply
            SaveAndUnloadRegister(CpuRegister::DX, SaveReason::Inside);

            ZeroRegister(CpuRegister::DX, 4);

            if (op2_reg != CpuRegister::None) {
                // Register to register copy
                uint8_t* a = AllocateBufferForInstruction(3);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0xF7;   // div r32, rm32
                a[2] = ToXrm(3, 6, op2_reg);
            } else if (!op2->symbol->parent) {
                // Static to register copy (16-bit range)
                uint8_t* a = AllocateBufferForInstruction(3 + 2);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0xF7;   // div r32, rm32
                a[2] = ToXrm(0, 6, 6);

                BackpatchStatic(a + 3, op2);
            } else {
                // Stack to register copy (8-bit range)
                uint8_t* a = AllocateBufferForInstruction(3 + 1);
                a[0] = 0x66;   // Operand size prefix
                a[1] = 0xF7;   // div r32, rm32
                a[2] = ToXrm(1, 6, 6);

                BackpatchLocal(a + 3, op2);
            }

            dst->reg = (i->assignment.type == AssignType::Remainder ? CpuRegister::DX : CpuRegister::AX);
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    dst->is_dirty = true;
    dst->last_used = ip_src;
}

void DosExeEmitter::EmitAssignShift(InstructionEntry* i)
{
    DosVariableDescriptor* dst = FindVariableByName(i->assignment.dst_value);

    int32_t dst_size = compiler->GetSymbolTypeSize(dst->symbol->type);

    switch (i->assignment.op2.exp_type) {
        case ExpressionType::Constant: {
            int32_t shift = atoi(i->assignment.op2.value);

            if (i->assignment.op1.exp_type == ExpressionType::Constant) {
                // Shift constant with constant
                int32_t value = atoi(i->assignment.op1.value);

                if (i->assignment.type == AssignType::ShiftLeft) {
                    value = value << shift;
                } else {
                    value = value >> shift;
                }

                CpuRegister reg_dst = GetUnusedRegister();
                LoadConstantToRegister(value, reg_dst, dst_size);

                dst->reg = reg_dst;
                dst->is_dirty = true;
                dst->last_used = ip_src;
                return;
            }

            SaveAndUnloadRegister(CpuRegister::CL, SaveReason::Inside);
            LoadConstantToRegister(shift, CpuRegister::CL, 1);
            // ToDo: Use shl/shr rm8/16/32, imm8
            break;
        }
        case ExpressionType::Variable: {
            DosVariableDescriptor* op2 = FindVariableByName(i->assignment.op2.value);
            CopyVariableToRegister(op2, CpuRegister::CL, 1);
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    // One operand is already in CL
    SuppressRegister _(this, CpuRegister::CL);

    CpuRegister reg_dst;
    switch (i->assignment.op1.exp_type) {
        case ExpressionType::Constant: {
            int32_t value = atoi(i->assignment.op1.value);

            reg_dst = GetUnusedRegister();
            LoadConstantToRegister(value, reg_dst, dst_size);
            break;
        }
        case ExpressionType::Variable: {
            DosVariableDescriptor* op1 = FindVariableByName(i->assignment.op1.value);
            int32_t op1_size = compiler->GetSymbolTypeSize(op1->symbol->type);

            if (dst == op1 && op1->reg != CpuRegister::None && dst_size <= op1_size) {
                reg_dst = op1->reg;
            } else {
                reg_dst = LoadVariableUnreferenced(op1, dst_size);
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    uint8_t type = (i->assignment.type == AssignType::ShiftLeft ? 4 : 5);

    switch (dst_size) {
        case 1: {
            uint8_t* a = AllocateBufferForInstruction(2);
            a[0] = 0xD2;    // shl/shr rm8, cl
            a[1] = ToXrm(3, type, reg_dst);
            break;
        }
        case 2: {
            uint8_t* a = AllocateBufferForInstruction(2);
            a[0] = 0xD3;    // shl/shr rm16, cl
            a[1] = ToXrm(3, type, reg_dst);
            break;
        }
        case 4: {
            uint8_t* a = AllocateBufferForInstruction(3);
            a[0] = 0x66;    // Operand size prefix
            a[1] = 0xD3;    // shl/shr rm32, cl
            a[2] = ToXrm(3, type, reg_dst);
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    dst->reg = reg_dst;
    dst->is_dirty = true;
    dst->last_used = ip_src;
}

void DosExeEmitter::EmitGoto(InstructionEntry* i)
{
    // Cannot jump to itself, this should not happen,
    // because "goto" instructions are generated by compiler
    if (i->goto_statement.ip == ip_src) {
        ThrowOnUnreachableCode();
    }

    // Jumps to the next instruction are removed automatically as optimization
    if (i->goto_statement.ip == ip_src + 1) {
        return;
    }

    // Unload all registers before jump
    SaveAndUnloadAllRegisters(SaveReason::Before);

    uint8_t* goto_ptr = nullptr;

    bool goto_near;
    if (i->goto_statement.ip < ip_src) {
        // Jump target was already emitted, check if we can use 8-bit address
        int32_t rel = (int32_t)(ip_src_to_dst[i->goto_statement.ip] - (ip_dst + 2));
        goto_near = (rel > INT8_MIN && rel < INT8_MAX);
    } else {
        // Not emitted yet, use estimation
        int32_t rel = (int32_t)(i->goto_statement.ip - ip_src) * NearJumpThreshold;
        goto_near = (rel > INT8_MIN && rel < INT8_MAX);
    }

    // Emit jump instruction
    if (goto_near) {
        uint8_t* a = AllocateBufferForInstruction(1 + 1);
        a[0] = 0xEB; // jmp rel8

        goto_ptr = (a + 1);
    } else {
        uint8_t* a = AllocateBufferForInstruction(1 + 2);
        a[0] = 0xE9; // jmp rel16

        goto_ptr = (a + 1);
    }

    if (i->goto_statement.ip < ip_src) {
        // Already defined
        int32_t rel = (int32_t)(ip_src_to_dst[i->goto_statement.ip] - ip_dst);

        if (goto_near) {
            if (rel < INT8_MIN || rel > INT8_MAX) {
                throw CompilerException(CompilerExceptionSource::Compilation,
                    "Compiler cannot generate that high relative address");
            }

            *(uint8_t*)goto_ptr = rel;
        } else {
            *(uint16_t*)goto_ptr = rel;
        }
    } else {
        // Create backpatch info, if the label was not defined yet
        DosBackpatchInstruction b { };
        b.type = (goto_near ? DosBackpatchType::ToRel8 : DosBackpatchType::ToRel16);
        b.backpatch_offset = (uint32_t)(goto_ptr - buffer);
        b.backpatch_ip = ip_dst;
        b.target = DosBackpatchTarget::IP;
        b.ip_src = i->goto_statement.ip;
        backpatch.push_back(b);
    }
}

void DosExeEmitter::EmitGotoLabel(InstructionEntry* i)
{
    // Try to find target label
    std::list<DosLabel>::iterator it = labels.begin();

    while (it != labels.end()) {
        if (strcmp(it->name, i->goto_label_statement.label) == 0) {
            break;
        }

        ++it;
    }

    // Unload all registers before jump
    SaveAndUnloadAllRegisters(SaveReason::Before);

    uint8_t* goto_ptr = nullptr;

    bool goto_near;
    if (it != labels.end()) {
        // Jump target was already emitted, check if we can use 8-bit address
        int32_t rel = (int32_t)(it->ip_dst - (ip_dst + 2));
        goto_near = (rel > INT8_MIN && rel < INT8_MAX);
    } else {
        // Not emitted yet, use estimation
        // ToDo: Implement estamination
        //int32_t rel = (int32_t)(i->if_statement.ip - ip_src) * NearJumpThreshold;
        //goto_near = (rel > INT8_MIN && rel < INT8_MAX);
        goto_near = false;
    }

    // Emit jump instruction
    if (goto_near) {
        uint8_t* a = AllocateBufferForInstruction(1 + 1);
        a[0] = 0xEB; // jmp rel8

        goto_ptr = (a + 1);
    } else {
        uint8_t* a = AllocateBufferForInstruction(1 + 2);
        a[0] = 0xE9; // jmp rel16

        goto_ptr = (a + 1);
    }

    if (it != labels.end()) {
        // Already defined
        int32_t rel = (int32_t)(it->ip_dst - ip_dst);

        if (goto_near) {
            if (rel < INT8_MIN || rel > INT8_MAX) {
                throw CompilerException(CompilerExceptionSource::Compilation,
                    "Compiler cannot generate that high relative address");
            }

            *(uint8_t*)goto_ptr = rel;
        } else {
            *(uint16_t*)goto_ptr = rel;
        }
    } else {
        // Create backpatch info, if the label was not defined yet
        DosBackpatchInstruction b { };
        b.type = (goto_near ? DosBackpatchType::ToRel8 : DosBackpatchType::ToRel16);
        b.backpatch_offset = (uint32_t)(goto_ptr - buffer);
        b.backpatch_ip = ip_dst;
        b.target = DosBackpatchTarget::Label;
        b.value = i->goto_label_statement.label;
        backpatch.push_back(b);
    }
}

void DosExeEmitter::EmitIf(InstructionEntry* i)
{
    // Cannot jump to itself, this should not happen,
    // because "goto" instructions are generated by compiler
    if (i->if_statement.ip == ip_src) {
        ThrowOnUnreachableCode();
    }

    // Jumps to the next instruction are removed automatically as optimization
    if (i->if_statement.ip == ip_src + 1) {
        return;
    }

    // Unload all registers before jump
    SaveAndUnloadAllRegisters(SaveReason::Before);

    uint8_t* goto_ptr = nullptr;

    bool goto_near;
    if (i->if_statement.ip < ip_src) {
        // Jump target was already emitted, check if we can use 8-bit address
        int32_t rel = (int32_t)(ip_src_to_dst[i->if_statement.ip] - (ip_dst + NearJumpThreshold));
        goto_near = (rel > INT8_MIN && rel < INT8_MAX);
    } else {
        // Not emitted yet, use estimation
        int32_t rel = (int32_t)(i->if_statement.ip - ip_src) * NearJumpThreshold;
        goto_near = (rel > INT8_MIN && rel < INT8_MAX);
    }

    if (i->if_statement.op1.exp_type == ExpressionType::Constant) {
        // Constant has to be second operand, swap them
        std::swap(i->if_statement.op1, i->if_statement.op2);

        i->if_statement.type = GetSwappedCompareType(i->if_statement.type);
    }

    if (i->if_statement.op1.type.base == BaseSymbolType::String || i->if_statement.op2.type.base == BaseSymbolType::String) {
        EmitIfStrings(i, goto_ptr, goto_near);
    } else {
        switch (i->if_statement.type) {
            case CompareType::LogOr:
            case CompareType::LogAnd: {
                EmitIfOrAnd(i, goto_ptr, goto_near);
                break;
            }

            case CompareType::Equal:
            case CompareType::NotEqual:
            case CompareType::Greater:
            case CompareType::Less:
            case CompareType::GreaterOrEqual:
            case CompareType::LessOrEqual: {
                EmitIfArithmetic(i, goto_ptr, goto_near);
                break;
            }

            default: ThrowOnUnreachableCode();
        }
    }

    if (!goto_ptr) {
        return;
    }

    if (i->if_statement.ip < ip_src) {
        int32_t rel = (int32_t)(ip_src_to_dst[i->if_statement.ip] - ip_dst);
        if (goto_near) {
            if (rel < INT8_MIN || rel > INT8_MAX) {
                throw CompilerException(CompilerExceptionSource::Compilation,
                    "Compiler cannot generate that high relative address");
            }

            *(uint8_t*)goto_ptr = rel;
        } else {
            *(uint16_t*)goto_ptr = rel;
        }
    } else {
        // Create backpatch info, if the line was not precessed yet
        DosBackpatchInstruction b { };
        b.type = (goto_near ? DosBackpatchType::ToRel8 : DosBackpatchType::ToRel16);
        b.backpatch_offset = (uint32_t)(goto_ptr - buffer);
        b.backpatch_ip = ip_dst;
        b.target = DosBackpatchTarget::IP;
        b.ip_src = i->if_statement.ip;
        backpatch.push_back(b);
    }
}

void DosExeEmitter::EmitIfOrAnd(InstructionEntry* i, uint8_t*& goto_ptr, bool& goto_near)
{
    switch (i->if_statement.op2.exp_type) {
        case ExpressionType::Constant: {
            switch (i->if_statement.op1.exp_type) {
                case ExpressionType::Constant: {
                    int32_t value1 = atoi(i->if_statement.op1.value);
                    int32_t value2 = atoi(i->if_statement.op2.value);

                    if (IfConstexpr(i->if_statement.type, value1, value2)) {
                        if (goto_near) {
                            uint8_t* a = AllocateBufferForInstruction(1 + 1);
                            a[0] = 0xEB;   // jmp rel8

                            goto_ptr = a + 1;
                        } else {
                            uint8_t* a = AllocateBufferForInstruction(1 + 2);
                            a[0] = 0xE9;   // jmp rel16

                            goto_ptr = a + 1;
                        }
                    }
                    break;
                }
                case ExpressionType::Variable: {
                    DosVariableDescriptor* op1 = FindVariableByName(i->if_statement.op1.value);

                    int32_t op1_size = compiler->GetSymbolTypeSize(op1->symbol->type);

                    int32_t value = atoi(i->if_statement.op2.value);

                    CpuRegister reg_dst = LoadVariableUnreferenced(op1, op1_size);

                    uint8_t type = (i->if_statement.type == CompareType::LogOr ? 1 : 0);

                    // ToDo: This should be max(op1_size, op2_size)
                    switch (op1_size) {
                        case 1: {
                            uint8_t* a = AllocateBufferForInstruction(2 + 1);
                            a[0] = 0x80;   // or/and rm8, imm8
                            a[1] = ToXrm(3, type, reg_dst);
                            *(uint8_t*)(a + 2) = (int8_t)value;
                            break;
                        }
                        case 2: {
                            uint8_t* a = AllocateBufferForInstruction(2 + 2);
                            a[0] = 0x81;   // or/and rm16, imm16
                            a[1] = ToXrm(3, type, reg_dst);
                            *(uint16_t*)(a + 2) = (int16_t)value;
                            break;
                        }
                        case 4: {
                            uint8_t* a = AllocateBufferForInstruction(3 + 4);
                            a[0] = 0x66;   // Operand size prefix
                            a[1] = 0x81;   // or/and rm32, imm32
                            a[2] = ToXrm(3, type, reg_dst);
                            *(uint32_t*)(a + 3) = (int32_t)value;
                            break;
                        }

                        default: ThrowOnUnreachableCode();
                    }
                    break;
                }

                default: ThrowOnUnreachableCode();
            }
            break;
        }

        case ExpressionType::Variable: {
            DosVariableDescriptor* op1 = FindVariableByName(i->if_statement.op1.value);
            DosVariableDescriptor* op2 = FindVariableByName(i->if_statement.op2.value);

            if (op2->reg != CpuRegister::None) {
                // If the second operand is already in register, swap them.
                // If not, it doesn't matter, one operand has to be in register.
                std::swap(op1, op2);
            }

            int32_t op1_size = compiler->GetSymbolTypeSize(op1->symbol->type);

            CpuRegister reg_dst = LoadVariableUnreferenced(op1, op1_size);

            // ToDo: This should be max(op1_size, op2_size)
            switch (op1_size) {
                case 1: {
                    uint8_t opcode = (i->if_statement.type == CompareType::LogOr ? 0x0A : 0x22);
                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(2);
                        a[0] = opcode; // or/and r8, rm8
                        a[1] = ToXrm(3, reg_dst, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 2);
                        a[0] = opcode; // or/and r8, rm8
                        a[1] = ToXrm(0, reg_dst, 6);

                        BackpatchStatic(a + 2, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 1);
                        a[0] = opcode; // or/and r8, rm8
                        a[1] = ToXrm(1, reg_dst, 6);

                        BackpatchLocal(a + 2, op2);
                    }
                    break;
                }
                case 2: {
                    uint8_t opcode = (i->if_statement.type == CompareType::LogOr ? 0x0B : 0x23);
                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(2);
                        a[0] = opcode; // or/and r16, rm16
                        a[1] = ToXrm(3, reg_dst, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 2);
                        a[0] = opcode; // or/and r16, rm16
                        a[1] = ToXrm(0, reg_dst, 6);

                        BackpatchStatic(a + 2, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 1);
                        a[0] = opcode; // or/and r16, rm16
                        a[1] = ToXrm(1, reg_dst, 6);

                        BackpatchLocal(a + 2, op2);
                    }
                    break;
                }
                case 4: {
                    uint8_t opcode = (i->if_statement.type == CompareType::LogOr ? 0x0B : 0x23);
                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(3);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = opcode; // or/and r32, rm32
                        a[2] = ToXrm(3, reg_dst, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(3 + 2);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = opcode; // or/and r32, rm32
                        a[2] = ToXrm(0, reg_dst, 6);

                        BackpatchStatic(a + 3, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(3 + 1);
                        a[0] = 0x66;   // Operand size prefix
                        a[1] = opcode; // or/and r32, rm32
                        a[2] = ToXrm(1, reg_dst, 6);
                        
                        BackpatchLocal(a + 3, op2);
                    }
                    break;
                }

                default: ThrowOnUnreachableCode();
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    if (goto_near) {
        uint8_t* a = AllocateBufferForInstruction(1 + 1);
        a[0] = 0x75; // jnz rel8

        goto_ptr = a + 1;
    } else{
        uint8_t* a = AllocateBufferForInstruction(2 + 2);
        a[0] = 0x0F;
        a[1] = 0x85; // jnz rel16 (i386+)

        goto_ptr = a + 2;
    }
}

void DosExeEmitter::EmitIfArithmetic(InstructionEntry* i, uint8_t*& goto_ptr, bool& goto_near)
{
    switch (i->if_statement.op2.exp_type) {
        case ExpressionType::Constant: {
            switch (i->if_statement.op1.exp_type) {
                case ExpressionType::Constant: {
                    int32_t value1 = atoi(i->if_statement.op1.value);
                    int32_t value2 = atoi(i->if_statement.op2.value);

                    if (IfConstexpr(i->if_statement.type, value1, value2)) {
                        if (goto_near) {
                            uint8_t* a = AllocateBufferForInstruction(1 + 1);
                            a[0] = 0xEB;   // jmp rel8

                            goto_ptr = a + 1;
                        } else {
                            uint8_t* a = AllocateBufferForInstruction(1 + 2);
                            a[0] = 0xE9;   // jmp rel16

                            goto_ptr = a + 1;
                        }
                    }
                    break;
                }
                case ExpressionType::Variable: {
                    DosVariableDescriptor* op1 = FindVariableByName(i->if_statement.op1.value);
                    int32_t op1_size = compiler->GetSymbolTypeSize(op1->symbol->type);

                    int32_t value = atoi(i->if_statement.op2.value);

                    CpuRegister reg_dst = LoadVariableUnreferenced(op1, op1_size);

                    // ToDo: This should be max(op1_size, op2_size)
                    switch (op1_size) {
                        case 1: {
                            uint8_t* a = AllocateBufferForInstruction(2 + 1);
                            a[0] = 0x80;    // cmp rm8, imm8
                            a[1] = ToXrm(3, 7, reg_dst);
                            *(uint8_t*)(a + 2) = (int8_t)value;
                            break;
                        }
                        case 2: {
                            uint8_t* a = AllocateBufferForInstruction(2 + 2);
                            a[0] = 0x81;    // cmp rm16, imm16
                            a[1] = ToXrm(3, 7, reg_dst);
                            *(uint16_t*)(a + 2) = (int16_t)value;
                            break;
                        }
                        case 4: {
                            uint8_t* a = AllocateBufferForInstruction(3 + 4);
                            a[0] = 0x66;    // Operand size prefix
                            a[1] = 0x81;    // cmp rm32, imm32
                            a[2] = ToXrm(3, 7, reg_dst);
                            *(uint32_t*)(a + 3) = (int32_t)value;
                            break;
                        }

                        default: ThrowOnUnreachableCode();
                    }
                    break;
                }

                default: ThrowOnUnreachableCode();
            }
            break;
        }

        case ExpressionType::Variable: {
            DosVariableDescriptor* op1 = FindVariableByName(i->if_statement.op1.value);
            DosVariableDescriptor* op2 = FindVariableByName(i->if_statement.op2.value);

            if (op2->reg != CpuRegister::None) {
                // If the second operand is already in register, swap them
                // If not, it doesn't matter, one operand has to be in register
                std::swap(op1, op2);

                i->if_statement.type = GetSwappedCompareType(i->if_statement.type);
            }

            int32_t op1_size = compiler->GetSymbolTypeSize(op1->symbol->type);

            CpuRegister reg_dst = LoadVariableUnreferenced(op1, op1_size);

            // ToDo: This should be max(op1_size, op2_size)
            switch (op1_size) {
                case 1: {
                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(2);
                        a[0] = 0x3A;    // cmp r8, rm8
                        a[1] = ToXrm(3, reg_dst, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 2);
                        a[0] = 0x3A;    // cmp r8, rm8
                        a[1] = ToXrm(0, reg_dst, 6);

                        BackpatchStatic(a + 2, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 1);
                        a[0] = 0x3A;    // cmp r8, rm8
                        a[1] = ToXrm(1, reg_dst, 6);

                        BackpatchLocal(a + 2, op2);
                    }
                    break;
                }
                case 2: {
                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(2);
                        a[0] = 0x3B;    // cmp r16, rm16
                        a[1] = ToXrm(3, reg_dst, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 2);
                        a[0] = 0x3B;    // cmp r16, rm16
                        a[1] = ToXrm(0, reg_dst, 6);

                        BackpatchStatic(a + 2, op2);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(2 + 1);
                        a[0] = 0x3B;    // cmp r16, rm16
                        a[1] = ToXrm(1, reg_dst, 6);

                        BackpatchLocal(a + 2, op2);
                    }
                    break;
                }
                case 4: {
                    if (op2->reg != CpuRegister::None) {
                        // Register to register copy
                        uint8_t* a = AllocateBufferForInstruction(3);
                        a[0] = 0x66;    // Operand size prefix
                        a[1] = 0x3B;    // cmp r32, rm32
                        a[2] = ToXrm(3, reg_dst, op2->reg);
                    } else if (!op2->symbol->parent) {
                        // Static to register copy (16-bit range)
                        uint8_t* a = AllocateBufferForInstruction(3 + 2);
                        a[0] = 0x66;    // Operand size prefix
                        a[1] = 0x3B;    // cmp r32, rm32
                        a[2] = ToXrm(1, reg_dst, 6);

                        BackpatchStatic(a + 3, op1);
                    } else {
                        // Stack to register copy (8-bit range)
                        uint8_t* a = AllocateBufferForInstruction(3 + 1);
                        a[0] = 0x66;    // Operand size prefix
                        a[1] = 0x3B;    // cmp r32, rm32
                        a[2] = ToXrm(1, reg_dst, 6);

                        BackpatchLocal(a + 3, op2);
                    }
                    break;
                }

                default: ThrowOnUnreachableCode();
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    if (goto_ptr) {
        // Jump instruction was already emitted
        return;
    }

    uint8_t opcode;
    switch (i->if_statement.type) {
        case CompareType::Equal:          opcode = 0x74; break; // jz rel
        case CompareType::NotEqual:       opcode = 0x75; break; // jnz rel
        case CompareType::Greater:        opcode = 0x77; break; // jnbe rel
        case CompareType::Less:           opcode = 0x72; break; // jb rel
        case CompareType::GreaterOrEqual: opcode = 0x73; break; // jnb rel
        case CompareType::LessOrEqual:    opcode = 0x76; break; // jbe rel

        default: ThrowOnUnreachableCode();
    }

    if (goto_near) {
        uint8_t* a = AllocateBufferForInstruction(1 + 1);
        a[0] = opcode;

        goto_ptr = (a + 1);
    } else {
        uint8_t* a = AllocateBufferForInstruction(2 + 2);
        a[0] = 0x0F;
        a[1] = opcode + 0x10; // (i386+)

        goto_ptr = (a + 2);
    }
}

void DosExeEmitter::EmitIfStrings(InstructionEntry* i, uint8_t*& goto_ptr, bool& goto_near)
{
    if (i->if_statement.op1.type != i->if_statement.op2.type) {
        ThrowOnUnreachableCode();
    }

    if (i->if_statement.op1.exp_type == ExpressionType::Constant) {
        // Constant string comparison
        bool result = (strcmp(i->if_statement.op1.value, i->if_statement.op2.value) == 0);
        if (i->if_statement.type == CompareType::NotEqual) {
            result = !result;
        } else if (i->if_statement.type != CompareType::Equal) {
            ThrowOnUnreachableCode();
        }

        if (result) {
            if (goto_near) {
                uint8_t* a = AllocateBufferForInstruction(1 + 1);
                a[0] = 0xEB;   // jmp rel8

                goto_ptr = a + 1;
            } else {
                uint8_t* a = AllocateBufferForInstruction(1 + 2);
                a[0] = 0xE9;   // jmp rel16

                goto_ptr = a + 1;
            }
        }
        return;
    }

    if (i->if_statement.op2.exp_type == ExpressionType::Constant) {
        strings.insert(i->if_statement.op2.value);

        uint8_t* a = AllocateBufferForInstruction(1 + 2);
        a[0] = 0x68;    // push imm16

        // Create backpatch info for string
        BackpatchString(a + 1, i->if_statement.op2.value);
    } else {
        DosVariableDescriptor* op2 = FindVariableByName(i->if_statement.op2.value);
        PushVariableToStack(op2, compiler->GetSymbolTypeSize({ BaseSymbolType::String, 0 }));
    }

    DosVariableDescriptor* op1 = FindVariableByName(i->if_statement.op1.value);
    PushVariableToStack(op1, compiler->GetSymbolTypeSize({ BaseSymbolType::String, 0 }));

    // IP of shared function means reference count
    SymbolTableEntry* symbol = compiler->GetSymbols();
    while (symbol) {
        if (symbol->type.base == BaseSymbolType::SharedFunction && strcmp(symbol->name, "#StringsEqual") == 0) {
            symbol->ref_count++;
            break;
        }

        symbol = symbol->next;
    }

    // Emit "call" instruction
    {
        uint8_t* call = AllocateBufferForInstruction(1 + 2);
        call[0] = 0xE8; // call rel16

        // Create backpatch info, because of shared function
        {
            DosBackpatchInstruction b { };
            b.type = DosBackpatchType::ToRel16;
            b.backpatch_offset = (uint32_t)((call + 1) - buffer);
            b.backpatch_ip = ip_dst;
            b.target = DosBackpatchTarget::Function;
            b.value = "#StringsEqual";
            backpatch.push_back(b);
        }
    }

    // Check result
    uint8_t* l1 = AllocateBufferForInstruction(2);
    l1[0] = 0x08;   // or rm8, r8
    l1[1] = ToXrm(3, CpuRegister::AX, CpuRegister::AX);

    uint8_t opcode;
    if (i->if_statement.type == CompareType::NotEqual) {
        opcode = 0x74; // jz rel
    } else if (i->if_statement.type == CompareType::Equal) {
        opcode = 0x75; // jnz rel
    } else {
        ThrowOnUnreachableCode();
    }

    if (goto_near) {
        uint8_t* l2 = AllocateBufferForInstruction(1 + 1);
        l2[0] = opcode;

        goto_ptr = (l2 + 1);
    } else {
        uint8_t* l2 = AllocateBufferForInstruction(2 + 2);
        l2[0] = 0x0F;
        l2[1] = opcode + 0x10; // (i386+)

        goto_ptr = (l2 + 2);
    }
}

void DosExeEmitter::EmitPush(InstructionEntry* i, std::stack<InstructionEntry*>& call_parameters)
{
    call_parameters.push(i);
}

void DosExeEmitter::EmitCall(InstructionEntry* i, SymbolTableEntry* symbol_table, std::stack<InstructionEntry*>& call_parameters)
{
    // Parameter count mismatch, this should not happen,
    // because "call" instructions are generated by compiler
    if (i->call_statement.target->parameter != call_parameters.size()) {
        ThrowOnUnreachableCode();
    }

    // Emit "push" instructions (evaluated right to left)
    {
        for (int32_t param = i->call_statement.target->parameter; param > 0; param--) {
            InstructionEntry* push = call_parameters.top();
            call_parameters.pop();

            SymbolTableEntry* param_decl = symbol_table;
            while (param_decl) {
                if (param_decl->parameter == param && param_decl->parent &&
                    strcmp(param_decl->parent, i->call_statement.target->name) == 0) {

                    break;
                }

                param_decl = param_decl->next;
            }

            // Can't find parameter, this should not happen,
            // because function parameters are generated by compiler
            if (!param_decl) {
                ThrowOnUnreachableCode();
            }

            switch (push->push_statement.symbol->exp_type) {
                case ExpressionType::Constant: {
                    // Push constant directly to parameter stack
                    switch (param_decl->type.base) {
                        case BaseSymbolType::Bool:
                        case BaseSymbolType::Uint8: {
                            uint8_t imm8 = atoi(push->push_statement.symbol->name);

                            uint8_t* a = AllocateBufferForInstruction(1 + 1);
                            a[0] = 0x6A;    // push imm8
                            a[1] = imm8;
                            break;
                        }
                        case BaseSymbolType::Uint16: {
                            uint16_t imm16 = atoi(push->push_statement.symbol->name);

                            uint8_t* a = AllocateBufferForInstruction(1 + 2);
                            a[0] = 0x68;    // push imm16
                            *(uint16_t*)(a + 1) = imm16;
                            break;
                        }
                        case BaseSymbolType::Uint32: {
                            uint32_t imm32 = atoi(push->push_statement.symbol->name);

                            uint8_t* a = AllocateBufferForInstruction(2 + 4);
                            a[0] = 0x66;    // Operand size prefix
                            a[1] = 0x68;    // push imm32
                            *(uint32_t*)(a + 2) = imm32;
                            break;
                        }

                        case BaseSymbolType::String: {
                            strings.insert(push->push_statement.symbol->name);

                            uint8_t* a = AllocateBufferForInstruction(1 + 2);
                            a[0] = 0x68;    // push imm16

                            // Create backpatch info for string
                            BackpatchString(a + 1, push->push_statement.symbol->name);
                            break;
                        }

                        //case BaseSymbolType::Array: break;

                        default: ThrowOnUnreachableCode();
                    }

                    break;
                }

                case ExpressionType::Variable: {
                    DosVariableDescriptor* var = FindVariableByName(push->push_statement.symbol->name);
                    PushVariableToStack(var, compiler->GetSymbolTypeSize(param_decl->type));
                    break;
                }

                default: ThrowOnUnreachableCode();
            }
        }
    }

    SaveAndUnloadAllRegisters(SaveReason::Inside);

    // Emit "call" instruction
    {
        uint8_t* call = AllocateBufferForInstruction(1 + 2);
        call[0] = 0xE8; // call rel16

        std::list<DosLabel>::iterator it = functions.begin();

        while (it != functions.end()) {
            if (strcmp(it->name, i->call_statement.target->name) == 0) {
                *(uint16_t*)(call + 1) = (int16_t)(it->ip_dst - ip_dst);
                goto AlreadyPatched;
            }

            ++it;
        }

        // Create backpatch info, if the function was not defined yet
        {
            DosBackpatchInstruction b { };
            b.type = DosBackpatchType::ToRel16;
            b.backpatch_offset = (uint32_t)((call + 1) - buffer);
            b.backpatch_ip = ip_dst;
            b.target = DosBackpatchTarget::Function;
            b.value = i->call_statement.target->name;
            backpatch.push_back(b);
        }

    AlreadyPatched:
        ;
    }

    if (i->call_statement.target->return_type.base != BaseSymbolType::Void || i->call_statement.target->return_type.pointer != 0) {
        // Set register of return variable to AX
        DosVariableDescriptor* ret = FindVariableByName(i->call_statement.return_symbol);
        ret->reg = CpuRegister::AX;
        ret->is_dirty = true;
        ret->last_used = ip_src;
    }
}

void DosExeEmitter::EmitReturn(InstructionEntry* i, SymbolTableEntry* symbol_table)
{
    was_return = true;

    bool are_types_compatible = ((!i && parent->return_type.base == BaseSymbolType::Void && parent->return_type.pointer == 0) ||
                                 i->return_statement.op.type == parent->return_type);
    if (!are_types_compatible) {
        are_types_compatible = compiler->GetLargestTypeForArithmetic(
            i->return_statement.op.type, parent->return_type).base != BaseSymbolType::Unknown;
    }

    if (!are_types_compatible) {
        std::string message = "All returns in function \"";
        message += parent->name;
        message += "\" must return \"";
        message += compiler->BaseSymbolTypeToString(parent->return_type.base);
        message += "\" value, found \"";
        message += compiler->BaseSymbolTypeToString(i->return_statement.op.type.base);
        message += "\" instead";
        throw CompilerException(CompilerExceptionSource::Statement, message);
    }

    if (parent->type.base == BaseSymbolType::EntryPoint) {
        // Entry point is handled differently,
        // DOS Function Dispatcher is caled at the end of the function,
        // return value is passed to DOS and the program is terminated
        switch (i->return_statement.op.exp_type) {
            case ExpressionType::Constant: {
                uint8_t imm8 = atoi(i->return_statement.op.value);

                uint8_t* a = AllocateBufferForInstruction(2);
                a[0] = 0xB0;    // mov al, imm8
                a[1] = imm8;    //  Return code
                break;
            }
            case ExpressionType::Variable: {
                DosVariableDescriptor* src = FindVariableByName(i->return_statement.op.value);

                if (src->reg == CpuRegister::AX) {
                    // Value is already in place, no need to do anything
                } else if (src->reg != CpuRegister::None) {
                    // Register to register copy
                    uint8_t* a = AllocateBufferForInstruction(2);
                    a[0] = 0x8A;    // mov r8, rm8
                    a[1] = ToXrm(3, CpuRegister::AL, src->reg);
                } else if (!src->symbol->parent) {
                    // Static to register copy (16-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 2);
                    a[0] = 0x8A;    // mov r8, rm8
                    a[1] = ToXrm(0, CpuRegister::AL, 6);

                    BackpatchStatic(a + 2, src);
                } else {
                    // Stack to register copy (8-bit range)
                    uint8_t* a = AllocateBufferForInstruction(2 + 1);
                    a[0] = 0x8A;    // mov r8, rm8
                    a[1] = ToXrm(1, CpuRegister::AL, 6);

                    BackpatchLocal(a + 2, src);
                }
                break;
            }

            default: ThrowOnUnreachableCode();
        }

        AsmInt(0x21 /*DOS Function Dispatcher*/, 0x4C /*Terminate Process With Return Code*/);
    } else {
        // Standard function with "stdcall" calling convention,
        // return value (if any) is saved in AX register
        if (parent->return_type.base != BaseSymbolType::Void || parent->return_type.pointer != 0) {
            int32_t dst_size = compiler->GetSymbolTypeSize(parent->return_type);

            switch (i->return_statement.op.exp_type) {
                case ExpressionType::Constant: {
                    int32_t value = atoi(i->return_statement.op.value);
                    LoadConstantToRegister(value, CpuRegister::AX, dst_size);
                    break;
                }
                case ExpressionType::Variable: {
                    DosVariableDescriptor* src = FindVariableByName(i->return_statement.op.value);
                    CopyVariableToRegister(src, CpuRegister::AX, dst_size);
                    break;
                }

                default: ThrowOnUnreachableCode();
            }
        }

        // Destroy current call frame
        if (parent->parameter > 0) {
            // Compute needed space in stack for parameters, 
            // so stack region with parameters can be released
            uint16_t stack_param_size = 0;

            SymbolTableEntry* param_decl = symbol_table;
            while (param_decl) {
                if (param_decl->parameter != 0 && param_decl->parent && strcmp(param_decl->parent, parent->name) == 0) {
                    int32_t size = compiler->GetSymbolTypeSize(param_decl->type);
                    if (size < 2) {
                        size = 2;
                    }
                    stack_param_size += size;
                }

                param_decl = param_decl->next;
            }

            AsmProcLeave(stack_param_size, true);
        } else {
            AsmProcLeave(0);
        }
    }
}

CompareType DosExeEmitter::GetSwappedCompareType(CompareType type)
{
    switch (type) {
        case CompareType::Equal:          return CompareType::Equal;
        case CompareType::NotEqual:       return CompareType::NotEqual;
        case CompareType::Greater:        return CompareType::Less;
        case CompareType::Less:           return CompareType::Greater;
        case CompareType::GreaterOrEqual: return CompareType::LessOrEqual;
        case CompareType::LessOrEqual:    return CompareType::GreaterOrEqual;

        default: ThrowOnUnreachableCode();
    }
}

bool DosExeEmitter::IfConstexpr(CompareType type, int32_t op1, int32_t op2)
{
    switch (type) {
        case CompareType::LogOr:          return (op1 || op2);
        case CompareType::LogAnd:         return (op1 && op2);

        case CompareType::Equal:          return (op1 == op2);
        case CompareType::NotEqual:       return (op1 != op2);
        case CompareType::Greater:        return (op1 > op2);
        case CompareType::Less:           return (op1 < op2);
        case CompareType::GreaterOrEqual: return (op1 >= op2);
        case CompareType::LessOrEqual:    return (op1 <= op2);

        default: ThrowOnUnreachableCode();
    }
}

void DosExeEmitter::EmitSharedFunction(char* name, std::function<void()> emitter)
{
    SymbolTableEntry* symbol = compiler->GetSymbols();

    while (symbol) {
        if (symbol->type.base == BaseSymbolType::SharedFunction && strcmp(symbol->name, name) == 0) {
            if (symbol->ref_count > 0) {
                Log::Write(LogType::Info, "Emitting \"%s\"...", name);

                // Function is referenced
                BackpatchLabels({ name, ip_dst }, DosBackpatchTarget::Function);

                emitter();
            } else {
                // Function is not referened
            }
            return;
        }

        symbol = symbol->next;
    }

    ThrowOnUnreachableCode();
}