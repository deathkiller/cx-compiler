#pragma once

#include <stdint.h>

#include "SymbolTableEntry.h"

enum struct InstructionType {
    Unknown,
    Nop,

    Assign,
    Goto,
    GotoLabel,
    If,
    Push,
    Call,
    Return,
};

enum struct AssignType {
    // One operand
    None,
    Negation,

    // Two operands
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    ShiftLeft,
    ShiftRight,
};

enum struct CompareType {
    None,

    LogOr,
    LogAnd,

    Equal,
    NotEqual,
    Greater,
    Less,
    GreaterOrEqual,
    LessOrEqual
};

struct InstructionOperandIndex {
    char* value;
    SymbolType type;
    ExpressionType exp_type;
};

struct InstructionOperand {
    char* value;
    SymbolType type;
    ExpressionType exp_type;
    InstructionOperandIndex index;
};

struct InstructionEntry {
    InstructionType type;

    union {
        struct {
            AssignType type;

            char* dst_value;
            InstructionOperandIndex dst_index;

            InstructionOperand op1;
            InstructionOperand op2;
        } assignment;

        struct {
            int32_t ip;
        } goto_statement;

        struct {
            char* label;
        } goto_label_statement;

        struct {
            int32_t ip;

            CompareType type;

            InstructionOperand op1;
            InstructionOperand op2;
        } if_statement;
        
        struct {
            SymbolTableEntry* symbol;
        } push_statement;

        struct {
            SymbolTableEntry* target;
            char* return_symbol;
        } call_statement;

        struct {
            InstructionOperand op;
        } return_statement;
    };

    InstructionEntry* next;
};

struct BackpatchList {
    InstructionEntry* entry;

    BackpatchList* next;
};

struct SwitchBackpatchList {
    uint32_t source_ip;
    bool is_default;
    char* value;
    SymbolType type;

    uint32_t line;

    SwitchBackpatchList* next;
};