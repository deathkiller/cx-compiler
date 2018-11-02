#include "Compiler.h"

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <string>

// Windows-specific includes
#include "targetver.h"
#include <windows.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi")

#include "Version.h"
#include "Log.h"
#include "DosExeEmitter.h"

// Internal Bison functions and variables used by compiler
extern int yylex();
extern int yyparse();
extern FILE* yyin;
extern int yylineno;


Compiler::Compiler()
{
}

Compiler::~Compiler()
{
}

int Compiler::OnRun(int argc, wchar_t* argv[])
{
    if (argc < 2) {
        Log::Write(LogType::Error, "You must specify at least output filename!");
        return EXIT_FAILURE;
    }

    int32_t argIdx = 0;
    wchar_t* input_filename = nullptr;
    wchar_t* output_filename = nullptr;

    for (int i = 1; i < argc; i++) {
        wchar_t* value;
        if (StringStartsWith(argv[i], L"/target:", value)) {
            if (wcscmp(value, L"dos") == 0) {
                // Nothing to do for now...
            } else {
                Log::Write(LogType::Error, "Unsupported compilation target specified!");
                return EXIT_FAILURE;
            }
        } else {
            switch (argIdx) {
                case 0: input_filename = argv[i]; break;
                case 1: output_filename = argv[i]; break;
            }

            argIdx++;
        }
    }

    // Open input file
    if (argIdx == 0) {
        Log::Write(LogType::Error, "You must specify at least output filename!");
        return EXIT_FAILURE;
    } else if (argIdx == 1) {
        yyin = stdin;

        output_filename = input_filename;
        input_filename = nullptr;
    } else {
        errno_t err = _wfopen_s(&yyin, input_filename, L"rb");
        if (err) {
            char error[200];
            strerror_s(error, err);
            Log::Write(LogType::Error, "Error while opening input file: %s", error);
            return EXIT_FAILURE;
        }
    }

    // Open output files
    FILE* outputExe;
    errno_t err = _wfopen_s(&outputExe, output_filename, L"wb");
    if (err) {
        char error[200];
        strerror_s(error, err);
        Log::Write(LogType::Error, "Error while creating output file: %s", error);

        if (yyin && yyin != stdin) {
            fclose(yyin);
        }
        
        return EXIT_FAILURE;
    }

    // Set working directory
    if (input_filename && PathRemoveFileSpec(input_filename)) {
        SetCurrentDirectory(input_filename);

        wchar_t path[MAX_PATH];
        GetCurrentDirectory(MAX_PATH, path);
    }

    // Declare all shared functions
    DeclareSharedFunctions();

    bool input_done = false;

    // Parse input file
    try {
        if (input_filename) {
            Log::Write(LogType::Info, "Parsing source code...");
        } else {
            Log::Write(LogType::Info, "");
            Log::Write(LogType::Info, "- " VERSION_NAME " - v" VERSION_FILEVERSION);
            Log::Write(LogType::Info, "");
            Log::Write(LogType::Info, "Compiling application in interactive mode (press CTRL-Z to compile):");
        }
        Log::PushIndent();

        if (!input_filename) {
            Log::WriteSeparator();
            Log::SetHighlight(true);
        }

        do {
            yyparse();
        } while (!feof(yyin));

        if (!input_filename) {
            Log::SetHighlight(false);
            Log::WriteSeparator();
        }

        input_done = true;

        Log::PopIndent();

        PostprocessSymbolTable();

        Log::Write(LogType::Info, "Creating executable file...");
        Log::PushIndent();

        // Parsing was successful, generate output files
        {
            DosExeEmitter emitter(this);
            emitter.EmitMzHeader();
            emitter.EmitInstructions(instruction_stream_head);
            emitter.EmitSharedFunctions();
            emitter.EmitStaticData();
            emitter.FixMzHeader(instruction_stream_head, stack_size);
            emitter.Save(outputExe);
        }

        Log::PopIndent();
        Log::Write(LogType::Info, "Build was successful!");
    } catch (CompilerException& ex) {
        // Input file can't be parsed/compiled

        // Cleanup
        if (!input_done && !input_filename) {
            Log::SetHighlight(false);
            Log::WriteSeparator();
        }

        if (yyin && yyin != stdin) {
            fclose(yyin);
        }

        fclose(outputExe);

        // Show error message
        const char* source;
        switch (ex.GetSource()) {
            case CompilerExceptionSource::Syntax:      source = "Syntax: ";      break;
            case CompilerExceptionSource::Declaration: source = "Declaration: "; break;
            case CompilerExceptionSource::Statement:   source = "Statement: ";   break;

            default: source = ""; break;
        }

        int32_t line = ex.GetLine();
        if (line >= 0) {
            int32_t column = ex.GetColumn();
            if (column >= 0) {
                Log::Write(LogType::Error, "[%d:%d] %s%s", line, column, source, ex.what());
            } else {
                Log::Write(LogType::Error, "[%d:-] %s%s", line, source, ex.what());
            }
        } else {
            Log::Write(LogType::Error, "%s%s", source, ex.what());
        }

        Log::PopIndent();
        Log::PopIndent();
        Log::Write(LogType::Error, "Build failed!");

        return EXIT_FAILURE;
    }

    if (yyin != stdin) {
        fclose(yyin);
    }

    fclose(outputExe);

    ReleaseAll();

    return EXIT_SUCCESS;
}

void Compiler::ParseCompilerDirective(char* directive, std::function<bool(char* directive, char* param)> callback)
{
    char* param = directive;
    while (*param && *param != ' ' && *param != '\r' && *param != '\n') {
        param++;
    }

    if (*param == ' ') {
        *param = '\0';
        param++;

        while (*param == ' ') {
            param++;
        }

        if (*param && *param != '\r' && *param != '\n') {
            char* param_end = param;
            while (*param_end && *param_end != '\r' && *param_end != '\n') {
                param_end++;
            }

            *param_end = '\0';

            // Parameter provided
            if (strcmp(directive, "#stack") == 0) {
                // Stack size directive
                if (*param == '^') {
                    uint32_t new_stack_size = atoi(param + 1);
                    if (stack_size < new_stack_size) {
                        stack_size = new_stack_size;
                    }
                } else {
                    stack_size = atoi(param);
                }
                return;
            }

            if (callback(directive, param)) {
                return;
            }
        } else {
            if (callback(directive, nullptr)) {
                return;
            }
        }
    } else {
        if (callback(directive, nullptr)) {
            return;
        }
    }

    Log::Write(LogType::Warning, "Compiler directive \"%s\" cannot be resolved", directive);
}

InstructionEntry* Compiler::AddToStream(InstructionType type)
{
    InstructionEntry* entry = new InstructionEntry();
    entry->type = type;

    if (instruction_stream_head) {
        instruction_stream_tail->next = entry;
        instruction_stream_tail = entry;
    } else {
        instruction_stream_head = entry;
        instruction_stream_tail = entry;
    }

    // Advance abstract instruction pointer
    current_ip++;

    return entry;
}

BackpatchList* Compiler::AddToStreamWithBackpatch(InstructionType type)
{
    InstructionEntry* entry = AddToStream(type);

    BackpatchList* backpatch = new BackpatchList();
    backpatch->entry = entry;
    return backpatch;
}

void Compiler::BackpatchStream(BackpatchList* list, int32_t new_ip)
{
    while (list) {
        if (list->entry) {
            // Apply new abstract instruction pointer value
            if (list->entry->type == InstructionType::Goto) {
                list->entry->goto_statement.ip = new_ip;
            } else if (list->entry->type == InstructionType::If) {
                list->entry->if_statement.ip = new_ip;
            } else {
                // This type cannot be backpatched
                Log::Write(LogType::Error, "Trying to backpatch unsupported instruction");

                ThrowOnUnreachableCode();
            }
        }

        // Release entry in backpatch linked list
        BackpatchList* current = list;
        list = list->next;
        delete current;
    }
}

SymbolTableEntry* Compiler::GetSymbols()
{
    return symbol_table;
}

SymbolTableEntry* Compiler::ToDeclarationList(SymbolType type, int32_t size, const char* name, ExpressionType exp_type)
{
    SymbolTableEntry* symbol = new SymbolTableEntry();
    symbol->name = _strdup(name);
    symbol->type = type;
    symbol->size = size;
    symbol->exp_type = exp_type;

    if (declaration_queue) {
        SymbolTableEntry* entry = declaration_queue;
        while (entry->next) {
            if (strcmp(name, entry->name) == 0) {
                std::string message = "Variable \"";
                message += name;
                message += "\" is already declared in this scope";
                throw CompilerException(CompilerExceptionSource::Declaration, message, yylineno, -1);
            }
            entry = entry->next;
        }
        entry->next = symbol;
    } else {
        declaration_queue = symbol;
    }

    return symbol;
}

void Compiler::ToParameterList(SymbolType type, const char* name)
{
    parameter_count++;
    
    SymbolTableEntry* symbol = new SymbolTableEntry();
    symbol->name = _strdup(name);
    symbol->type = type;
    symbol->parameter = parameter_count;

    if (declaration_queue) {
        SymbolTableEntry* entry = declaration_queue;
        while (entry->next) {
            if (strcmp(name, entry->name) == 0) {
                std::string message = "Parameter \"";
                message += name;
                message += "\" is already declared in this scope";
                throw CompilerException(CompilerExceptionSource::Declaration, message, yylineno, -1);
            }
            entry = entry->next;
        }
        entry->next = symbol;
    } else {
        declaration_queue = symbol;
    }
}

SymbolTableEntry* Compiler::ToCallParameterList(SymbolTableEntry* list, SymbolType type, const char* name, ExpressionType exp_type)
{
    SymbolTableEntry* symbol = new SymbolTableEntry();
    symbol->name = _strdup(name);
    symbol->type = type;
    symbol->exp_type = exp_type;

    if (list) {
        SymbolTableEntry* entry = list;
        while (entry->next) {
            entry = entry->next;
        }
        entry->next = symbol;
        return list;
    } else {
        return symbol;
    }
}

void Compiler::AddLabel(const char* name, int32_t ip)
{
    SymbolTableEntry* symbol = new SymbolTableEntry();
    symbol->name = _strdup(name);
    symbol->type = { BaseSymbolType::Label, 0 };
    symbol->ip = ip;

    if (declaration_queue) {
        SymbolTableEntry* entry = declaration_queue;
        while (entry->next) {
            if (strcmp(name, entry->name) == 0) {
                std::string message = "Label \"";
                message += name;
                message += "\" is already declared in this scope";
                throw CompilerException(CompilerExceptionSource::Declaration, message, yylineno, -1);
            }
            entry = entry->next;
        }
        entry->next = symbol;
    } else {
        declaration_queue = symbol;
    }
}

void Compiler::AddStaticVariable(SymbolType type, int32_t size, const char* name)
{
    SymbolTableEntry* entry = AddSymbol(name, type, size, { BaseSymbolType::Unknown, 0 },
        ExpressionType::Variable, 0, 0, nullptr, false);
}

void Compiler::AddFunction(char* name, SymbolType return_type)
{
    // Check, if the function is not defined yet
    {
        SymbolTableEntry* current = symbol_table;
        while (current) {
            if ((current->type.base == BaseSymbolType::Function ||
                 current->type.base == BaseSymbolType::EntryPoint ||
                 current->type.base == BaseSymbolType::SharedFunction) &&
                strcmp(name, current->name) == 0) {

                std::string message = "Function \"";
                message += name;
                message += "\" is already defined";
                throw CompilerException(CompilerExceptionSource::Declaration, message, yylineno, -1);
            }

            current = current->next;
        }
    }

    int32_t ip = function_ip;
    function_ip = NextIp();

    uint32_t offset_internal = 0;

    if (strcmp(name, EntryPointName) == 0) {
        // Entry point found
        if (parameter_count != 0) {
            throw CompilerException(CompilerExceptionSource::Declaration, "Entry point must have zero parameters", yylineno, -1);
        }
        if (return_type.base != BaseSymbolType::Uint8 || return_type.pointer != 0) {
            throw CompilerException(CompilerExceptionSource::Declaration, "Entry point must return \"uint8\" value", yylineno, -1);
        }

        // Collect all variables used in the function
        SymbolTableEntry* current = declaration_queue;
        while (current) {
            AddSymbol(current->name, current->type, current->size, current->return_type,
                current->exp_type, current->ip, 0, name, current->is_temp);

            current = current->next;
        }

        SymbolTableEntry* entry = AddSymbol(name, { BaseSymbolType::EntryPoint, 0 }, 0, return_type,
            ExpressionType::None, ip, 0, nullptr, false);

        ReleaseDeclarationQueue();
        return;
    }
    
    // Find function prototype
    SymbolTableEntry* prototype = symbol_table;
    while (prototype) {
        if (prototype->type.base == BaseSymbolType::FunctionPrototype && strcmp(name, prototype->name) == 0) {
            break;
        }

        prototype = prototype->next;
    }

    if (prototype) {
        if ((!declaration_queue && parameter_count != 0) || prototype->parameter != parameter_count) {
            std::string message = "Parameter count does not match for function \"";
            message += name;
            message += "\"";
            throw CompilerException(CompilerExceptionSource::Declaration, message, yylineno, -1);
        }

        if (prototype->return_type != return_type) {
            std::string message = "Return type does not match for function \"";
            message += name;
            message += "\"";
            throw CompilerException(CompilerExceptionSource::Declaration, message, yylineno, -1);
        }

        // Promote the prototype to complete function
        prototype->type = { BaseSymbolType::Function, 0 };
        prototype->ip = ip;

        // Collect all function parameters
        SymbolTableEntry* current = symbol_table;
        for (uint16_t i = 0; i < parameter_count; i++) {
            while (!current->parent || strcmp(current->parent, name) != 0) {
                current = current->next;
                if (current->parent && strcmp(current->parent, name) == 0) {
                    // Parameter found
                    break;
                }
            }

            if (current->type != declaration_queue->type) {
                std::string message = "Parameter \"";
                message += current->name;
                message += "\" type does not match for function \"";
                message += name;
                message += "\"";
                throw CompilerException(CompilerExceptionSource::Declaration, message, yylineno, -1);
            }

            // Remove parameter from the queue
            SymbolTableEntry* remove = declaration_queue;
            declaration_queue = declaration_queue->next;
            delete remove;

            current = current->next;
        }

        // Collect all variables used in the function
        current = declaration_queue;
        while (current) {
            AddSymbol(current->name, current->type, current->size, current->return_type,
                current->exp_type, current->ip, 0, name, current->is_temp);

            current = current->next;
        }
    } else {
        // Prototype was not defined yet
        if (!declaration_queue && parameter_count != 0) {
            std::string message = "Parameter count does not match for function \"";
            message += name;
            message += "\"";
            throw CompilerException(CompilerExceptionSource::Declaration, message, yylineno, -1);
        }

        // Collect all function parameters and used variables
        uint16_t parameter_current = 0;
        SymbolTableEntry* current = declaration_queue;
        while (current) {
            if (parameter_current < parameter_count) {
                parameter_current++;
                current->parameter = parameter_current;
            } else {
                current->parameter = 0;
            }

            AddSymbol(current->name, current->type, current->size, current->return_type,
                current->exp_type, current->ip, current->parameter, name, current->is_temp);

            current = current->next;
        }

        AddSymbol(name, { BaseSymbolType::Function, 0 }, 0, return_type,
            ExpressionType::None, ip, parameter_count, nullptr, false);
    }

    ReleaseDeclarationQueue();
}

void Compiler::AddFunctionPrototype(char* name, SymbolType return_type)
{
    if (strcmp(name, EntryPointName) == 0) {
        throw CompilerException(CompilerExceptionSource::Declaration, "Prototype for entry point is not allowed", yylineno, -1);
    }
    if (!declaration_queue && parameter_count != 0) {
        throw CompilerException(CompilerExceptionSource::Declaration, "Parameter count does not match", yylineno, -1);
    }

    // Check if the function with the same name is already declared
    {
        SymbolTableEntry* current = symbol_table;
        while (current) {
            if ((current->type.base == BaseSymbolType::FunctionPrototype ||
                 current->type.base == BaseSymbolType::Function ||
                 current->type.base == BaseSymbolType::EntryPoint ||
                 current->type.base == BaseSymbolType::SharedFunction) &&
                strcmp(name, current->name) == 0) {

                std::string message = "Duplicate function definition for \"";
                message += current->name;
                message += "\"";
                throw CompilerException(CompilerExceptionSource::Declaration, message, yylineno, -1);
            }

            current = current->next;
        }
    }

    AddSymbol(name, { BaseSymbolType::FunctionPrototype, 0 }, 0, return_type,
        ExpressionType::None, 0, parameter_count, nullptr, false);

    // Collect all function parameters
    {
        uint16_t parameter_current = 0;
        SymbolTableEntry* current = declaration_queue;
        while (current) {
            parameter_current++;

            AddSymbol(current->name, current->type, current->size, current->return_type,
                current->exp_type, current->ip, parameter_current, name, current->is_temp);

            current = current->next;
        }
    }

    ReleaseDeclarationQueue();
}

void Compiler::PrepareForCall(const char* name, SymbolTableEntry* call_parameters, int32_t parameter_count)
{
    // Find function by its name
    SymbolTableEntry* current = symbol_table;
    while (current) {
        if ((current->type.base == BaseSymbolType::Function ||
             current->type.base == BaseSymbolType::FunctionPrototype ||
             current->type.base == BaseSymbolType::SharedFunction) &&
            strcmp(name, current->name) == 0) {

            break;
        }

        current = current->next;
    }

    if (!current) {
        std::string message = "Cannot call function \"";
        message += name;
        message += "\", because it was not declared";
        throw CompilerException(CompilerExceptionSource::Statement, message, yylineno, -1);
    }

    if (current->parameter != parameter_count) {
        std::string message = "Cannot call function \"";
        message += name;
        message += "\" because of parameter count mismatch";
        throw CompilerException(CompilerExceptionSource::Statement, message, yylineno, -1);
    }

    current = symbol_table;
    int32_t parameters_found = 0;

    do {
        // Find parameter description
        while (current) {
            if (current->parent && strcmp(name, current->parent) == 0 && current->parameter != 0) {
                break;
            }

            current = current->next;
        }

        if (!call_parameters || !current) {
            // No parameters found
            if (parameter_count != 0 || parameters_found != 0) {
                std::string message = "Cannot call function \"";
                message += name;
                message += "\" because of parameter count mismatch";
                throw CompilerException(CompilerExceptionSource::Statement, message, yylineno, -1);
            }

            return;
        }
        
        if (!CanImplicitCast(current->type, call_parameters->type, call_parameters->exp_type)) {
            std::string message = "Cannot call function \"";
            message += name;
            message += "\" because of parameter \"";
            message += current->name;
            message += "\" type mismatch";
            throw CompilerException(CompilerExceptionSource::Statement, message, yylineno, -1);
        }

        // Add required parameter to stream
        InstructionEntry* i = AddToStream(InstructionType::Push);
        i->push_statement.symbol = call_parameters;

        current = current->next;

        call_parameters = call_parameters->next;

        parameters_found++;
    } while (parameters_found < parameter_count);
}

SymbolTableEntry* Compiler::GetParameter(const char* name)
{
    // Search in function-local variable list
    SymbolTableEntry* current = declaration_queue;
    while (current) {
        if (strcmp(name, current->name) == 0) {
            return current;
        }

        current = current->next;
    }

    if (!current) {
        // Search in static variable list
        current = symbol_table;
        while (current) {
            if (!current->parent &&
                current->type.base != BaseSymbolType::Function &&
                current->type.base != BaseSymbolType::FunctionPrototype &&
                current->type.base != BaseSymbolType::EntryPoint &&
                current->type.base != BaseSymbolType::SharedFunction &&
                strcmp(name, current->name) == 0) {
                return current;
            }

            current = current->next;
        }
    }

    return nullptr;
}

SymbolTableEntry* Compiler::GetFunction(const char* name)
{
    SymbolTableEntry* current = symbol_table;
    while (current) {
        if ((current->type.base == BaseSymbolType::Function ||
             current->type.base == BaseSymbolType::FunctionPrototype ||
             current->type.base == BaseSymbolType::SharedFunction) &&
            strcmp(name, current->name) == 0) {

            return current;
        }

        current = current->next;
    }

    return nullptr;
}

SymbolTableEntry* Compiler::FindSymbolByName(const char* name)
{
    SymbolTableEntry* current = symbol_table;
    while (current) {
        if (current->name && strcmp(name, current->name) == 0) {
            break;
        }

        current = current->next;
    }

    return current;
}

InstructionEntry* Compiler::FindInstructionByIp(int32_t ip)
{
    InstructionEntry* current = instruction_stream_head;
    int32_t ip_current = 0;

    while (current) {
        if (ip_current == ip) {
            return current;
        }

        current = current->next;
        ip_current++;
    }

    return nullptr;
}

bool Compiler::CanImplicitCast(SymbolType to, SymbolType from, ExpressionType type)
{
    if (from == to) {
        return true;
    }

    if (from.pointer > 0 && to.pointer == 1 && to.base == BaseSymbolType::Void) {
        // Implicit cast from any pointer type to "void*"
        return true;
    }

    if (from.pointer == 1 && from.base == BaseSymbolType::Void && to.pointer > 0 && type == ExpressionType::Constant) {
        // Implicit cast "const void*" to any pointer (for null)
        return true;
    }

    if (type == ExpressionType::Constant) {
        if ((from.base == BaseSymbolType::Uint8 ||
             from.base == BaseSymbolType::Uint16 ||
             from.base == BaseSymbolType::Uint32) &&
            (to.base == BaseSymbolType::Uint8 ||
             to.base == BaseSymbolType::Uint16 ||
             to.base == BaseSymbolType::Uint32)) {

            // Constant implicit cast
            return true;
        }
    }

    if (to.pointer == 0 && from.pointer == 0 &&
        to.base >= BaseSymbolType::Bool && from.base >= BaseSymbolType::Bool &&
        to.base <= BaseSymbolType::Uint32 && from.base <= BaseSymbolType::Uint32) {

        if (to.base >= from.base) {
            // Only expansion (assign smaller to bigger type) is detected as implicit cast
            return true;
        }
    }

    return false;
}

bool Compiler::CanExplicitCast(SymbolType to, SymbolType from)
{
    if (from == to) {
        return true;
    }

    if (from.base == BaseSymbolType::Unknown || to.base == BaseSymbolType::Unknown ||
        from.base == BaseSymbolType::None || to.base == BaseSymbolType::None) {
        return false;
    }

    return true;
}

SymbolType Compiler::GetLargestTypeForArithmetic(SymbolType a, SymbolType b)
{
    if (!TypeIsValid(a) || !TypeIsValid(b))
        return { BaseSymbolType::Unknown, 0 };
    if (a.base == BaseSymbolType::String || b.base == BaseSymbolType::String)
        return { BaseSymbolType::Unknown, 0 };

    if (a.base == BaseSymbolType::Uint32 || b.base == BaseSymbolType::Uint32)
        return { BaseSymbolType::Uint32, max(a.pointer, b.pointer) };
    if (a.base == BaseSymbolType::Uint16 || b.base == BaseSymbolType::Uint16)
        return { BaseSymbolType::Uint16, max(a.pointer, b.pointer) };
    if (a.base == BaseSymbolType::Uint8 || b.base == BaseSymbolType::Uint8)
        return { BaseSymbolType::Uint8, max(a.pointer, b.pointer) };

    return { BaseSymbolType::Unknown, 0 };
}

int32_t Compiler::NextIp()
{
    return current_ip + 1;
}


SymbolTableEntry* Compiler::GetUnusedVariable(SymbolType type)
{
    char buffer[20];

    switch (type.base) {
        case BaseSymbolType::Bool: {
            var_count_bool++;
            sprintf_s(buffer, "#b_%d", var_count_bool);
            break;
        }
        case BaseSymbolType::Uint8: {
            var_count_uint8++;
            sprintf_s(buffer, "#ui8_%d", var_count_uint8);
            break;
        }
        case BaseSymbolType::Uint16: {
            var_count_uint16++;
            sprintf_s(buffer, "#ui16_%d", var_count_uint16);
            break;
        }
        case BaseSymbolType::Uint32: {
            var_count_uint32++;
            sprintf_s(buffer, "#ui32_%d", var_count_uint32);
            break;
        }
        case BaseSymbolType::String: {
            var_count_string++;
            sprintf_s(buffer, "#s_%d", var_count_uint32);
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    SymbolTableEntry* decl = ToDeclarationList(type, 0, buffer, ExpressionType::Variable);
    decl->is_temp = true;
    return decl;
}

const char* Compiler::BaseSymbolTypeToString(BaseSymbolType type)
{
    switch (type) {
        case BaseSymbolType::Function: return "Function";
        case BaseSymbolType::FunctionPrototype: return "Prototype";
        case BaseSymbolType::EntryPoint: return "EntryPoint";
        case BaseSymbolType::SharedFunction: return "SharedFun.";

        case BaseSymbolType::Label: return "Label";

        case BaseSymbolType::Bool: return "bool";
        case BaseSymbolType::Uint8: return "uint8";
        case BaseSymbolType::Uint16: return "uint16";
        case BaseSymbolType::Uint32: return "uint32";
        case BaseSymbolType::String: return "string";
        case BaseSymbolType::Void: return "void";

        default: return "-";
    }
}

SymbolTableEntry* Compiler::AddSymbol(const char* name, SymbolType type, int32_t size, SymbolType return_type,
    ExpressionType exp_type, int32_t ip, int32_t parameter, const char* parent, bool is_temp)
{
    if (!name || strlen(name) == 0) {
        throw CompilerException(CompilerExceptionSource::Declaration, "Symbol name must not be empty", yylineno, -1);
    }

    SymbolTableEntry* symbol = new SymbolTableEntry();
    symbol->name = _strdup(name);
    symbol->type = type;
    symbol->size = size;
    symbol->return_type = return_type;
    symbol->exp_type = exp_type;
    symbol->ip = ip;
    symbol->parameter = parameter;
    symbol->parent = (parent == nullptr ? nullptr : _strdup(parent));
    symbol->is_temp = is_temp;

    // Add it to the symbol table
    SymbolTableEntry* tail = symbol_table;
    if (tail) {
        while (tail->next) {
            tail = tail->next;
        }

        tail->next = symbol;
    } else {
        symbol_table = symbol;
    }

    return symbol;
}

const char* Compiler::ExpressionTypeToString(ExpressionType type)
{
    switch (type) {
        case ExpressionType::Constant: return "Const.";
        case ExpressionType::Variable: return "Var.";

        default: return "-";
    }
}

int32_t Compiler::GetSymbolTypeSize(SymbolType type)
{
    if (type.pointer > 0) {
        return 2; // 16-bit pointer
    }

    switch (type.base) {
        case BaseSymbolType::Bool: return 1;
        case BaseSymbolType::Uint8: return 1;
        case BaseSymbolType::Uint16: return 2;
        case BaseSymbolType::Uint32: return 4;

        case BaseSymbolType::String: return 2; // 16-bit pointer

        default: ThrowOnUnreachableCode();
    }
}


int8_t Compiler::SizeToShift(int32_t size)
{
    size >>= 1;

    int8_t bits = 0;
    while (size > 0) {
        size >>= 1;
        bits++;
    }
    return bits;
}

void Compiler::IncreaseScope(ScopeType type)
{
    switch (type) {
        case ScopeType::Assign: {
            assign_scope++;
            break;
        }

        case ScopeType::Break: {
            break_scope++;
            break_list.push_back(nullptr);
            break;
        }

        case ScopeType::Continue: {
            continue_scope++;
            continue_list.push_back(nullptr);
            break;
        }

        default: ThrowOnUnreachableCode();
    }
}

void Compiler::ResetScope(ScopeType type)
{
    switch (type) {
        case ScopeType::Assign: {
            assign_scope = 0;
            break;
        }

        default: ThrowOnUnreachableCode();
    }
}

bool Compiler::IsScopeActive(ScopeType type)
{
    switch (type) {
        case ScopeType::Assign: {
            return (assign_scope > 0);
        }

        default: ThrowOnUnreachableCode();
    }
}

void Compiler::BackpatchScope(ScopeType type, int32_t new_ip)
{
    switch (type) {
        case ScopeType::Break: {
            BackpatchStream(break_list[break_scope], new_ip);

            break_list[break_scope] = nullptr;
            break_scope--;
            break;
        }

        case ScopeType::Continue: {
            BackpatchStream(continue_list[continue_scope], new_ip);

            continue_list[continue_scope] = nullptr;
            continue_scope--;
            break;
        }

        default: ThrowOnUnreachableCode();
    }
}

bool Compiler::AddToScopeList(ScopeType type, BackpatchList* backpatch)
{
    switch (type) {
        case ScopeType::Break: {
            if (break_scope < 0) {
                return false;
            }

            size_t list_size = break_list.size();
            if (list_size == 0 || list_size == break_scope - 1) {
                break_list.push_back(backpatch);
            } else {
                break_list[break_scope] = MergeLists(backpatch, break_list[break_scope]);
            }
            break;
        }

        case ScopeType::Continue: {
            if (continue_scope < 0) {
                return false;
            }

            size_t list_size = continue_list.size();
            if (list_size == 0 || list_size == continue_scope - 1) {
                continue_list.push_back(backpatch);
            } else {
                continue_list[continue_scope] = MergeLists(backpatch, continue_list[continue_scope]);
            }
            break;
        }

        default: ThrowOnUnreachableCode();
    }

    return true;
}

void Compiler::ReleaseDeclarationQueue()
{
    while (declaration_queue) {
        SymbolTableEntry* current = declaration_queue;
        declaration_queue = declaration_queue->next;
        //free(current->name);
        delete current;
    }

    parameter_count = 0;
}

void Compiler::ReleaseAll()
{
    ReleaseDeclarationQueue();

    while (instruction_stream_head) {
        InstructionEntry* current = instruction_stream_head;
        instruction_stream_head = instruction_stream_head->next;
        delete current;
    }

    instruction_stream_tail = nullptr;

    while (symbol_table) {
        SymbolTableEntry* current = symbol_table;
        symbol_table = symbol_table->next;
        free(current->name);
        free(current->parent);
        delete current;
    }
}

void Compiler::PostprocessSymbolTable()
{
    if (!symbol_table) {
        return;
    }

    Log::Write(LogType::Info, "Post-processing the symbol table...");

    // Fix IP of first function
    SymbolTableEntry* symbol = symbol_table;
    while (symbol) {
        if (!symbol->parent &&
            (symbol->type.base == BaseSymbolType::Function ||
             symbol->type.base == BaseSymbolType::EntryPoint)) {

            if (symbol->ip == 0) {
                symbol->ip = 1;
                break;
            }
        }

        symbol = symbol->next;
    }

    // Find entry point and create dependency graph
    symbol = symbol_table;
    SymbolTableEntry* entry_point = nullptr;
    while (symbol) {
        if (!symbol->parent && symbol->type.base == BaseSymbolType::EntryPoint) {
            entry_point = symbol;
            break;
        }

        symbol = symbol->next;
    }

    if (!entry_point) {
        ThrowOnUnreachableCode();
    }

    std::stack<SymbolTableEntry*> dependency_stack { };
    dependency_stack.push(entry_point);

    do {
        symbol = dependency_stack.top();
        dependency_stack.pop();

        if (symbol->ref_count > 0) {
            // Function was already processed
            continue;
        }

        symbol->ref_count++;

        int32_t ip_start = symbol->ip;
        int32_t ip_current = ip_start;
        InstructionEntry* current = FindInstructionByIp(ip_current);
        while (current) {
            if (ip_current != ip_start) {
                symbol = symbol_table;
                while (symbol) {
                    if (symbol->ip == ip_current &&
                        (symbol->type.base == BaseSymbolType::Function || symbol->type.base == BaseSymbolType::EntryPoint)) {
                        goto FunctionEnd;
                    }
                    symbol = symbol->next;
                }
            }

            if (current->type == InstructionType::Call) {
                SymbolTableEntry* target = current->call_statement.target;
                if (target->type.base == BaseSymbolType::SharedFunction) {
                    target->ref_count++;
                } else {
                    dependency_stack.push(target);
                }
            }

            current = current->next;
            ip_current++;
        }
    FunctionEnd:
        ;
    } while (!dependency_stack.empty());
}

void Compiler::DeclareSharedFunctions()
{
    // void PrintUint32(uint32 value);
    AddSymbol("PrintUint32", { BaseSymbolType::SharedFunction, 0 }, 0, { BaseSymbolType::Void, 0 },
        ExpressionType::None, 0, 1, nullptr, false);

    AddSymbol("value", { BaseSymbolType::Uint32, 0 }, 0, { BaseSymbolType::Unknown, 0 },
        ExpressionType::None, 0, 1, "PrintUint32", false);

    // void PrintString(string value);
    AddSymbol("PrintString", { BaseSymbolType::SharedFunction, 0 }, 0, { BaseSymbolType::Void, 0 },
        ExpressionType::None, 0, 1, nullptr, false);

    AddSymbol("value", { BaseSymbolType::String, 0 }, 0, { BaseSymbolType::Unknown, 0 },
        ExpressionType::None, 0, 1, "PrintString", false);

    // void PrintNewLine();
    AddSymbol("PrintNewLine", { BaseSymbolType::SharedFunction, 0 }, 0, { BaseSymbolType::Void, 0 },
        ExpressionType::None, 0, 0, nullptr, false);

    // uint32 ReadUint32();
    AddSymbol("ReadUint32", { BaseSymbolType::SharedFunction, 0 }, 0, { BaseSymbolType::Uint32, 0 },
        ExpressionType::None, 0, 0, nullptr, false);

    // string GetCommandLine();
    AddSymbol("GetCommandLine", { BaseSymbolType::SharedFunction, 0 }, 0, { BaseSymbolType::String, 0 },
        ExpressionType::None, 0, 0, nullptr, false);

    // bool #StringsEqual(string a, string b);
    AddSymbol("#StringsEqual", { BaseSymbolType::SharedFunction, 0 }, 0, { BaseSymbolType::Bool, 0 },
        ExpressionType::None, 0, 2, nullptr, false);

    AddSymbol("a", { BaseSymbolType::String, 0 }, 0, { BaseSymbolType::Unknown, 0 },
        ExpressionType::None, 0, 1, "#StringsEqual", false);

    AddSymbol("b", { BaseSymbolType::String, 0 }, 0, { BaseSymbolType::Unknown, 0 },
        ExpressionType::None, 0, 2, "#StringsEqual", false);

    // void* #Alloc(uint32 bytes);
    AddSymbol("#Alloc", { BaseSymbolType::SharedFunction, 0 }, 0, { BaseSymbolType::Void, 1 },
        ExpressionType::None, 0, 1, nullptr, false);

    AddSymbol("bytes", { BaseSymbolType::Uint32, 0 }, 0, { BaseSymbolType::Unknown, 0 },
        ExpressionType::None, 0, 1, "#Alloc", false);

    // void release(void* ptr); - Should be used as keyword
    AddSymbol("release", { BaseSymbolType::SharedFunction, 0 }, 0, { BaseSymbolType::Void, 0 },
        ExpressionType::None, 0, 1, nullptr, false);

    AddSymbol("ptr", { BaseSymbolType::Void, 1 }, 0, { BaseSymbolType::Unknown, 0 },
        ExpressionType::None, 0, 1, "release", false);
}

/*bool Compiler::StringStartsWith(const char* str, const char* prefix)
{
    if (!*prefix) {
        return true;
    }

    char cs, cp;
    while ((cp = *prefix++) && (cs = *str++)) {
        if (cp != cs) {
            return false;
        }
    }

    return (cp ? false : true);
}*/

bool Compiler::StringStartsWith(wchar_t* str, const wchar_t* prefix, wchar_t*& result)
{
    if (!*prefix) {
        result = str;
        return true;
    }

    wchar_t cs, cp;
    while ((cp = *prefix++) && (cs = *str++)) {
        if (cp != cs) {
            return false;
        }
    }

    if (cp) {
        return false;
    } else {
        result = str;
        return true;
    }
}