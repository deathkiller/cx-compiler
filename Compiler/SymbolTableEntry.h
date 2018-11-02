#pragma once

#include <stdint.h>

/// <summary>
/// All available types of symbols
/// </summary>
enum struct BaseSymbolType {
    Unknown,
    None,

    Function,
    FunctionPrototype,
    EntryPoint,
    SharedFunction,

    Label,

    Void,
    Bool,
    Uint8,
    Uint16,
    Uint32,
    String
};

struct SymbolType {
    BaseSymbolType base;
    uint8_t pointer;
};

inline bool operator==(const SymbolType& lhs, const SymbolType& rhs)
{
    return lhs.base == rhs.base && lhs.pointer == rhs.pointer;
}

inline bool operator!=(const SymbolType& lhs, const SymbolType& rhs)
{
    return lhs.base != rhs.base || lhs.pointer != rhs.pointer;
}

enum struct ExpressionType {
    None,

    Constant,
    Variable
};

struct SymbolTableEntry {
    char* name;
    SymbolType type;
    SymbolType return_type;
    ExpressionType exp_type;

    int32_t size;
    
    int32_t ip, parameter;
    char* parent;
    bool is_temp;

    uint32_t ref_count;

    SymbolTableEntry* next;
};