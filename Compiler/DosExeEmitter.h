#pragma once

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <string>
#include <list>
#include <map>
#include <stack>
#include <unordered_set>
#include <functional>

#include "Compiler.h"
#include "InstructionEntry.h"
#include "SymbolTableEntry.h"
#include "i386Emitter.h"

enum struct DosBackpatchType {
    Unknown,

    ToRel8,     // Relative address (signed 8-bit)
    ToRel16,    // Relative address (16-bit)
    ToDsAbs16,  // Absolute address to DS segment (16-bit)
    ToStack8    // Relative address (signed 8-bit)
};

enum struct DosBackpatchTarget {
    Unknown,

    IP,         // Instruction pointer
    Label,      // Named label
    Function,   // Function
    String,     // String
    Local,      // Local variable
    Static      // Static variable
};

struct DosBackpatchInstruction {
    DosBackpatchType type;
    DosBackpatchTarget target;

    uint32_t backpatch_offset;
    uint32_t backpatch_ip;

    int32_t ip_src;
    char* value;
};

struct DosVariableDescriptor {
    SymbolTableEntry* symbol;

    char* value;

    i386::CpuRegister reg;
    int32_t location;

    uint32_t last_used;

    bool is_dirty;
    bool force_save;
};

struct DosLabel {
    char* name;
    int32_t ip_dst;
};

enum struct SaveReason {
    Before,     // Variable will be saved if it's referenced in current or one of the following instructions
    Inside,     // Variable will be saved if it's referenced in one of the following instructions
    Force       // Variable will be always saved to stack
};


#pragma pack(push, 1)

// MZ EXE Header
struct MzHeader {
    uint8_t signature[2]; // MZ
    uint16_t last_block_size;
    uint16_t block_count;
    uint16_t reloc_count;
    uint16_t header_paragraphs;
    uint16_t min_extra_paragraphs;
    uint16_t max_extra_paragraphs;
    uint16_t ss;
    uint16_t sp;
    uint16_t checksum;
    uint16_t ip;
    uint16_t cs;
    uint16_t reloc_table_offset;
    uint16_t overlay_count;
};

struct MzRelocEntry {
    uint16_t offset;
    uint16_t segment;
};

#pragma pack(pop)

// Defines to shorten the code
/// <summary>
/// Register local variable for backpatching
/// </summary>
#define BackpatchLocal(ptr, var)                                    \
    {                                                               \
        if (!(var)->location) {                                     \
            backpatch.push_back({                                   \
                DosBackpatchType::ToStack8, DosBackpatchTarget::Local,  \
                (uint32_t)((ptr) - buffer), 0, 0, (var)->symbol->name   \
            });                                                     \
            (var)->symbol->ref_count++;                             \
        } else {                                                    \
            *(ptr) = (int8_t)(var)->location;                       \
        }                                                           \
    }

/// <summary>
/// Register static variable for backpatching
/// </summary>
#define BackpatchStatic(ptr, var)                                   \
    {                                                               \
        backpatch.push_back({                                       \
            DosBackpatchType::ToDsAbs16, DosBackpatchTarget::Static,\
            (uint32_t)((ptr) - buffer), 0, 0, (var)->symbol->name   \
        });                                                         \
    }

/// <summary>
/// Register string reference for backpatching
/// </summary>
#define BackpatchString(ptr, str)                                   \
    {                                                               \
        strings.insert(str);                                        \
        backpatch.push_back({                                       \
            DosBackpatchType::ToDsAbs16, DosBackpatchTarget::String,\
            (uint32_t)((ptr) - buffer), 0, 0, str                   \
        });                                                         \
    }

/// <summary>
/// Class that emits 16-bit EXE executable for DOS (i386)
/// </summary>
class DosExeEmitter : i386::Emitter
{
    friend class SuppressRegister;

public:
    DosExeEmitter(Compiler* compiler);
    ~DosExeEmitter();

    void EmitMzHeader();
    void EmitInstructions(InstructionEntry* instruction_stream);
    void EmitSharedFunctions();
    void EmitStaticData();
    void FixMzHeader(InstructionEntry* instruction_stream, uint32_t stack_size);

    void Save(FILE* stream);

private:
    /// <summary>
    /// Add all variables from symbol table to internal list
    /// </summary>
    /// <param name="symbol_table">Symbol table</param>
    void CreateVariableList(SymbolTableEntry* symbol_table);

    /// <summary>
    /// Return unused/free register, if all registers are referenced,
    /// save and unreference least used register
    /// </summary>
    /// <returns>Unused register</returns>
    i386::CpuRegister GetUnusedRegister();

    /// <summary>
    /// Return unused/free register, if all registers are referenced,
    /// no register will be returned (None)
    /// </summary>
    /// <returns>Unused register; or None</returns>
    i386::CpuRegister TryGetUnusedRegister();
    
    /// <summary>
    /// Find variable specified by name in variable list
    /// </summary>
    /// <param name="name">Name of variable</param>
    /// <returns>Variable descriptor</returns>
    DosVariableDescriptor* FindVariableByName(char* name);

    /// <summary>
    /// Find next reference to variable
    /// </summary>
    /// <param name="var">Variable descriptor</param>
    /// <returns>Instruction with reference</returns>
    /// <param name="reason">Save reason</param>
    InstructionEntry* FindNextVariableReference(DosVariableDescriptor* var, SaveReason reason);

    /// <summary>
    /// Find the end of current function
    /// </summary>
    /// <param name="symbol_table">Symbol table</param>
    void RefreshParentEndIp(SymbolTableEntry* symbol_table);

    /// <summary>
    /// Save specified variable to stack, but keep it in register
    /// </summary>
    /// <param name="var">Variable descriptor</param>
    /// <param name="force">Force save (don't check references)</param>
    /// <param name="reason">Save reason</param>
    void SaveVariable(DosVariableDescriptor* var, SaveReason reason);

    /// <summary>
    /// Save value of register to destination specified by variable and index
    /// </summary>
    /// <param name="var">Variable descriptor</param>
    /// <param name="index">Index descriptor</param>
    /// <param name="reg_dst">Register with value</param>
    void SaveIndexedVariable(DosVariableDescriptor* var, InstructionOperandIndex& index, i386::CpuRegister reg_dst);

    /// <summary>
    /// Save variable which uses specified register and unreference it
    /// </summary>
    /// <param name="reg">Register to unreference</param>
    /// <param name="reason">Save reason</param>
    void SaveAndUnloadRegister(i386::CpuRegister reg, SaveReason reason);

    /// <summary>
    /// Save all unsaved variables to stack and unreference all registers
    /// </summary>
    /// <param name="reason">Save reason</param>
    void SaveAndUnloadAllRegisters(SaveReason reason);

    /// <summary>
    /// Destroy connection of variable with register
    /// If the variable is unsaved, compiler exception is thrown
    /// </summary>
    /// <param name="reg">Discarded register</param>
    void MarkRegisterAsDiscarded(i386::CpuRegister reg);

    /// <summary>
    /// Push value of variable to parameter stack 
    /// </summary>
    /// <param name="var">Variable to push</param>
    /// <param name="param_size">Size of parameter</param>
    void PushVariableToStack(DosVariableDescriptor* var, int32_t param_size);

    /// <summary>
    /// Force load value of variable to any register,
    /// if it is already in register, ownership will be removed
    /// </summary>
    /// <param name="var">Variable to copy</param>
    /// <param name="desired_size">Target size</param>
    /// <returns>Target register</returns>
    i386::CpuRegister LoadVariableUnreferenced(DosVariableDescriptor* var, int32_t desired_size);

    /// <summary>
    /// Load pointer of variable to any register
    /// </summary>
    /// <param name="var">Variable descriptor</param>
    /// <param name="force_reference">If true, load address of pointer to pointer</param>
    /// <returns>Target register</returns>
    i386::CpuRegister LoadVariablePointer(DosVariableDescriptor* var, bool force_reference);

    /// <summary>
    /// Load value of variable and index to any register
    /// </summary>
    /// <param name="var">Variable descriptor</param>
    /// <param name="index">Index descriptor</param>
    /// <param name="desired_size">Target size</param>
    /// <returns>Target register</returns>
    i386::CpuRegister LoadIndexedVariable(DosVariableDescriptor* var, InstructionOperandIndex& index, int32_t desired_size);

    /// <summary>
    /// Force copy value of variable to specified register
    /// </summary>
    /// <param name="var">Variable to copy</param>
    /// <param name="reg_dst">Target register</param>
    /// <param name="desired_size">Target size</param>
    void CopyVariableToRegister(DosVariableDescriptor* var, i386::CpuRegister reg_dst, int32_t desired_size);

    /// <summary>
    /// Load constant value to specified register with smallest possible size
    /// </summary>
    /// <param name="value">Constant value</param>
    /// <param name="reg">Target register</param>
    void LoadConstantToRegister(int32_t value, i386::CpuRegister reg);

    /// <summary>
    /// Load constant value to specified register with specified size
    /// </summary>
    /// <param name="value">Constant value</param>
    /// <param name="reg">Target register</param>
    /// <param name="desired_size">Specified size</param>
    void LoadConstantToRegister(int32_t value, i386::CpuRegister reg, int32_t desired_size);

    /// <summary>
    /// Fill specified register with zeros
    /// </summary>
    /// <param name="reg">Register</param>
    /// <param name="desired_size">Size of register</param>
    void ZeroRegister(i386::CpuRegister reg, int32_t desired_size);

    // Backpatching
    /// <summary>
    /// Backpatch all entries in list with address of current line
    /// </summary>
    void BackpatchAddresses();

    /// <summary>
    /// Backpatch all entries in list with address of newly created label
    /// </summary>
    /// <param name="label">New label</param>
    /// <param name="target">Type of entries</param>
    void BackpatchLabels(const DosLabel& label, DosBackpatchTarget target);

    /// <summary>
    /// Check if there is no unresolved entries in backpatch list
    /// </summary>
    /// <param name="target">Type of entries</param>
    void CheckBackpatchListIsEmpty(DosBackpatchTarget target);

    /// <summary>
    /// Check if the last statement of function is return,
    /// so the function is terminated properly.
    /// If it's void function, return is emitted automatically.
    /// </summary>
    void CheckReturnStatementPresent();

    /// <summary>
    /// Process all events that are connected to symbol table entries
    /// </summary>
    /// <param name="symbol_table">Symbol table</param>
    void ProcessSymbolLinkage(SymbolTableEntry* symbol_table);

    // Instruction emitters
    void EmitEntryPointPrologue(SymbolTableEntry* function);

    void EmitFunctionPrologue(SymbolTableEntry* function, SymbolTableEntry* symbol_table);

    void EmitFunctionEpilogue();

    void EmitAssign(InstructionEntry* i);
    inline void EmitAssignNone(InstructionEntry* i);
    inline void EmitAssignNegation(InstructionEntry* i);
    inline void EmitAssignAddSubtract(InstructionEntry* i);
    inline void EmitAssignMultiply(InstructionEntry* i);
    inline void EmitAssignDivide(InstructionEntry* i);
    inline void EmitAssignShift(InstructionEntry* i);

    void EmitGoto(InstructionEntry* i);
    void EmitGotoLabel(InstructionEntry* i);

    void EmitIf(InstructionEntry* i);
    inline void EmitIfOrAnd(InstructionEntry* i, uint8_t*& goto_ptr, bool& goto_near);
    inline void EmitIfArithmetic(InstructionEntry* i, uint8_t*& goto_ptr, bool& goto_near);
    inline void EmitIfStrings(InstructionEntry* i, uint8_t*& goto_ptr, bool& goto_near);

    void EmitPush(InstructionEntry* i, std::stack<InstructionEntry*>& call_parameters);
    void EmitCall(InstructionEntry* i, SymbolTableEntry* symbol_table, std::stack<InstructionEntry*>& call_parameters);
    void EmitReturn(InstructionEntry* i, SymbolTableEntry* symbol_table);

    /// <summary>
    /// Get opposite compare type, so operands can be swapped
    /// </summary>
    /// <param name="type">Compare type</param>
    /// <returns>Opposite compare type</returns>
    CompareType GetSwappedCompareType(CompareType type);

    /// <summary>
    /// Do compare of two constant values at compile-time
    /// </summary>
    /// <param name="type">Compare type</param>
    /// <param name="op1">Operand 1</param>
    /// <param name="op2">Operand 2</param>
    /// <returns></returns>
    bool IfConstexpr(CompareType type, int32_t op1, int32_t op2);

    /// <summary>
    /// Emit shared function if it's referenced in source code
    /// </summary>
    /// <param name="name">Name of function</param>
    /// <param name="emitter">Callback to emit instructions</param>
    void EmitSharedFunction(char* name, std::function<void()> emitter);


    /// <summary>
    /// Max. number of abstract instructions that can fit into "rel8" address
    /// </summary>
    const int32_t NearJumpThreshold = 10;


    Compiler* compiler;

    int32_t ip_src = 0;

    int32_t static_size = 0;

    std::map<uint32_t, uint32_t> ip_src_to_dst;
    std::list<DosBackpatchInstruction> backpatch;
    std::list<DosVariableDescriptor> variables;
    std::list<DosLabel> functions;
    std::list<DosLabel> labels;
    std::unordered_set<char*> strings;

    std::unordered_set<i386::CpuRegister> suppressed_registers;
    
    SymbolTableEntry* parent = nullptr;
    int32_t parent_end_ip = 0;
    uint32_t parent_stack_offset = 0;
    InstructionEntry* current_instruction = nullptr;
    bool was_return = false;
};