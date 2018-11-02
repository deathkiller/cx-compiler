%{

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <string>

#include "Log.h"
#include "Compiler.h"

void yyerror(const char* s);

extern int yylex();
extern int yyparse();
extern FILE* yyin;
extern int yylineno;
extern char* yytext;

extern Compiler c;

%}

%locations
%define parse.error verbose

%token CONST STATIC VOID BOOL UINT8 UINT16 UINT32 STRING CONSTANT IDENTIFIER
%token IF ELSE RETURN DO WHILE FOR SWITCH CASE DEFAULT CONTINUE BREAK GOTO CAST ALLOC
%token INC_OP DEC_OP U_PLUS U_MINUS  
%token EQUAL NOT_EQUAL GREATER_OR_EQUAL LESS_OR_EQUAL SHIFT_LEFT SHIFT_RIGHT LOG_AND LOG_OR

%right '='
%left LOG_OR    
%left LOG_AND
%left '<' '>' LESS_OR_EQUAL GREATER_OR_EQUAL
%left EQUAL NOT_EQUAL
%left SHIFT_LEFT SHIFT_RIGHT
%left '+' '-'
%left '*' '/' '%'
%right U_PLUS U_MINUS '!'
%left INC_OP DEC_OP

%union
{
    char* string;
    int32_t integer;
    SymbolType type;

    struct {
        SymbolType type;
        int32_t size;
    } declaration;

    struct {
        char* value;
        SymbolType type;
        ExpressionType exp_type;
        
        struct {
            char* value;
            SymbolType type;
            ExpressionType exp_type;
        } index;

        BackpatchList* true_list;
        BackpatchList* false_list;
    } expression;

    struct {
        BackpatchList* next_list;
    } statement;

    struct {
        SwitchBackpatchList* next_list;
    } switch_statement;

    struct {
        SymbolTableEntry* list;
        int32_t count;
    } call_parameters;

    struct {
        int32_t ip;
        BackpatchList* next_list;
    } marker;
}

%type <string> id IDENTIFIER
%type <type> declaration_type
%type <declaration> declaration static_declaration
%type <expression> expression assignment_with_declaration assignment CONSTANT
%type <statement> statement statement_list matched_statement unmatched_statement program function_body function
%type <switch_statement> switch_statement case_list default_statement
%type <call_parameters> call_parameter_list
%type <marker> marker jump_marker continue_marker switch_next

%start program_head

%%

program_head
    : program
        {
            SymbolTableEntry* entry_point = c.FindSymbolByName(EntryPointName);
            if (!entry_point) {
                throw CompilerException(CompilerExceptionSource::Declaration,
                    "Entry point \"uint8 " EntryPointName "()\" not found");
            }

            int32_t entry_ip;
            if (entry_point->ip == 0) {
                entry_ip = 1;
            } else {
                entry_ip = entry_point->ip;
            }

            c.BackpatchStream($1.next_list, entry_ip);
        }
    ;

program
    : jump_marker function
        {
            LogDebug("P: Processing single function");

            $$.next_list = $1.next_list;
            c.BackpatchStream($2.next_list, c.NextIp());
        }
    | program function
        {
            LogDebug("P: Processing function in function list");

            $$.next_list = $1.next_list;
            c.BackpatchStream($2.next_list, c.NextIp());
        }
    ;


// Parts of function declaration
function
    : declaration_type id '(' parameter_list ')' ';'
        {
            c.AddFunctionPrototype($2, $1);

            // Nothing to backpatch here...
            $$.next_list = nullptr;
        }
    | declaration_type id '(' parameter_list ')' function_body
        {
            c.AddFunction($2, $1);
            $$.next_list = $6.next_list;
        }
    | static_declaration_list
        {
            // Nothing to backpatch here...
            $$.next_list = nullptr;
        }
    ;

function_body
    : '{' statement_list  '}'
        {
            LogDebug("P: Found function body");

            $$.next_list = $2.next_list;
        }
    ;

parameter_list
    : declaration_type id
        {
            c.ToParameterList($1, $2);

            LogDebug("P: Found parameter");
        }
    | parameter_list ',' declaration_type id
        {
            c.ToParameterList($3, $4);

            LogDebug("P: Found parameter list (end)");
        }
    | VOID
        {
            LogDebug("P: Found VOID parameter");
        }
    |
        {
            LogDebug("P: Found no parameter");
        }
    ;

// Types that can be used for variable declarations and function parameters
declaration_type
    : declaration_type '*'
        {
            $$ = $1;
            $$.pointer++;
        }
    | BOOL
        {
            $$ = { BaseSymbolType::Bool, 0 };

            LogDebug("P: Found BOOL declaration_type");
        }
    | UINT8
        {
            $$ = { BaseSymbolType::Uint8, 0 };

            LogDebug("P: Found UINT8 declaration_type");
        }
    | UINT16
        {
            $$ = { BaseSymbolType::Uint16, 0 };

            LogDebug("P: Found UINT16 declaration_type");
        }
    | UINT32
        {
            $$ = { BaseSymbolType::Uint32, 0 };

            LogDebug("P: Found UINT32 declaration_type");
        }
    | STRING
        {
            $$ = { BaseSymbolType::String, 0 };

            LogDebug("P: Found STRING declaration_type");
        }
    | VOID
        {
            $$ = { BaseSymbolType::Void, 0 };

            LogDebug("P: Found VOID declaration_type");
        }
    ;


// Statements
statement_list
    : statement
        {
            LogDebug("P: Processing single statement in statement list");

            $$.next_list = $1.next_list;
        }
    | statement_list marker statement
        {
            LogDebug("P: Processing statement list");

            c.BackpatchStream($1.next_list, $2.ip);
            $$.next_list = $3.next_list;
        }
    ;

statement
    : matched_statement
        {
            LogDebug("P: Processing matched statement");

            $$.next_list = $1.next_list;
        }
    | unmatched_statement
        {
            LogDebug("P: Processing unmatched statement");

            $$.next_list = $1.next_list;
        }
    | declaration_list
        {
            LogDebug("P: Processing declaration list");

            // Nothing to backpatch here...
            $$.next_list = nullptr;
        }
    | goto_label
        {
            LogDebug("P: Processing goto label");

            // Nothing to backpatch here...
            $$.next_list = nullptr;
        }
    ;

matched_statement
    : IF '(' assignment ')' marker matched_statement jump_marker ELSE marker matched_statement
        {
            LogDebug("P: Processing matched if..else");

            CheckIsIntOrBool($3, "Only integer and bool types are allowed in \"if\" statement", @3);

            CheckIsIfCompatible($3, "Unsupported expression for \"if\" statement", @3);

            c.BackpatchStream($3.true_list, $5.ip);
            c.BackpatchStream($3.false_list, $9.ip);
            $$.next_list = MergeLists($7.next_list, $10.next_list);
            $$.next_list = MergeLists($$.next_list, $6.next_list);
        }
    | assignment_with_declaration ';'
        {
            LogDebug("P: Processing matched assignment");

            // Nothing to backpatch here...
            $$.next_list = nullptr;
        }
    | RETURN ';'
        {
            LogDebug("P: Processing void return");

            $$.next_list = nullptr;
            InstructionEntry* i = c.AddToStream(InstructionType::Return);
            i->return_statement.op.type = { BaseSymbolType::None, 0 };
            i->return_statement.op.exp_type = ExpressionType::None;
        }
    | RETURN assignment ';'
        {
            LogDebug("P: Processing value return");

            $$.next_list = nullptr;
            InstructionEntry* i = c.AddToStream(InstructionType::Return);
            CopyOperand(i->return_statement.op, $2);
        }
    | WHILE continue_marker '(' assignment ')' marker break_marker matched_statement jump_marker marker
        {
            LogDebug("P: Processing matched while");

            CheckIsIntOrBool($4, "Only integer and bool types are allowed in \"while\" statement", @4);

            CheckIsIfCompatible($4, "Unsupported expression for \"while\" statement", @4);

            c.BackpatchStream($4.true_list, $6.ip);
            $$.next_list = $4.false_list;
            c.BackpatchStream($8.next_list, $2.ip);
            c.BackpatchStream($9.next_list, $2.ip);

            c.BackpatchScope(ScopeType::Continue, $2.ip);
            c.BackpatchScope(ScopeType::Break, $10.ip);
        }
    | DO continue_marker break_marker statement WHILE '(' marker assignment ')' ';' marker
        {
            LogDebug("P: Processing matched do..while");

            CheckIsIntOrBool($8, "Only integer and bool types are allowed in \"do..while\" statement", @8);

            CheckIsIfCompatible($8, "Unsupported expression for \"do..while\" statement", @8);

            c.BackpatchStream($4.next_list, $7.ip);
            c.BackpatchStream($8.true_list, $2.ip);
            $$.next_list = $8.false_list;

            c.BackpatchScope(ScopeType::Continue, $2.ip);
            c.BackpatchScope(ScopeType::Break, $11.ip);
        }
    | FOR '(' assignment_with_declaration ';' marker assignment ';' continue_marker assignment jump_marker ')' marker break_marker matched_statement jump_marker marker
        {
            LogDebug("P: Processing matched for");

            CheckIsInt($3, "Integer assignment is required in the first part of \"for\" statement", @3);
            CheckIsBool($6, "Bool expression is required in the middle part of \"for\" statement", @6);
            CheckIsInt($9, "Integer assignment is required in the last part of \"for\" statement", @9);

            CheckIsIfCompatible($6, "Unsupported expression for \"for\" statement", @6);

            c.BackpatchStream($3.true_list, $5.ip);
            c.BackpatchStream($14.next_list, $8.ip);
            c.BackpatchStream($15.next_list, $8.ip);
            $$.next_list = $6.false_list;
            c.BackpatchStream($6.true_list, $12.ip);
            c.BackpatchStream($9.true_list, $5.ip);
            c.BackpatchStream($10.next_list, $5.ip);

            c.BackpatchScope(ScopeType::Continue, $8.ip);
            c.BackpatchScope(ScopeType::Break, $16.ip);
        }
    | SWITCH '(' switch_next assignment ')' '{' break_marker switch_statement '}' switch_next
        {
            LogDebug("P: Processing switch statement");
        
            CheckIsInt($4, "Only integer types are allowed in \"switch\" statement", @4);

            SwitchBackpatchList* current = $8.next_list;
            SwitchBackpatchList* default_statement = nullptr;

            int32_t start_ip = c.NextIp();

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($4);

            while (current) {
                if (current->is_default) {
                    if (default_statement) {
                        ThrowOnUnreachableCode();
                    }

                    default_statement = current;
                } else {
                    InstructionEntry* i = c.AddToStream(InstructionType::If);
                    i->if_statement.ip = current->source_ip;

                    i->if_statement.type = CompareType::Equal;
                    i->if_statement.op1.value = $4.value;
                    i->if_statement.op1.type = $4.type;
                    i->if_statement.op1.exp_type = $4.exp_type;
                    i->if_statement.op2.value = current->value;
                    i->if_statement.op2.type = current->type;
                    i->if_statement.op2.exp_type = ExpressionType::Constant;
                }
                current = current->next;
            }

            if (default_statement) {
                InstructionEntry* i = c.AddToStream(InstructionType::Goto);
                i->goto_statement.ip = default_statement->source_ip;
            }

            int32_t end_ip = c.NextIp();

            c.BackpatchStream($3.next_list, start_ip);      // Backpatch start of "switch" statement
            c.BackpatchStream($10.next_list, end_ip);       // Backpatch end of "switch" statement

            c.BackpatchScope(ScopeType::Break, end_ip);     // Backpatch all break statement(s)

            $$.next_list = nullptr;
        }
    | BREAK ';'
        {
            BackpatchList* b = c.AddToStreamWithBackpatch(InstructionType::Goto);

            if (!c.AddToScopeList(ScopeType::Break, b)) {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "Break is not inside a loop or switch statement", @1.first_line, @1.first_column);
            }

            $$.next_list = nullptr;
        }
    | CONTINUE ';'
        {
            BackpatchList* b = c.AddToStreamWithBackpatch(InstructionType::Goto);

            if (!c.AddToScopeList(ScopeType::Continue, b)) {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "Continue is not inside a loop statement", @1.first_line, @1.first_column);
            }

            $$.next_list = nullptr;
        }
    | GOTO id ';'
        {
            LogDebug("P: Processing goto command");

            $$.next_list = nullptr;

            InstructionEntry* i = c.AddToStream(InstructionType::GotoLabel);
            i->goto_label_statement.label = $2;
        }
    | '{' statement_list '}'
        {
            LogDebug("P: Processing statement block");

            $$.next_list = $2.next_list;
        }
    | '{' '}'
        {        
            LogDebug("P: Processing empty block");

            $$.next_list = NULL;
        }
    ;

unmatched_statement
    : IF '(' assignment ')' marker statement
        {
            LogDebug("P: Processing unmatched if");

            CheckIsIntOrBool($3, "Only integer and bool types are allowed in \"if\" statement", @3);

            CheckIsIfCompatible($3, "Unsupported expression for \"if\" statement", @3);

            c.BackpatchStream($3.true_list, $5.ip);
            $$.next_list = MergeLists($3.false_list, $6.next_list);
        }
    | WHILE continue_marker '(' assignment ')' marker break_marker unmatched_statement jump_marker marker
        {
            LogDebug("P: Processing unmatched while");

            CheckIsIntOrBool($4, "Only integer and bool types are allowed in \"while\" statement", @4);

            CheckIsIfCompatible($4, "Unsupported expression for \"while\" statement", @4);

            c.BackpatchStream($4.true_list, $6.ip);
            $$.next_list = $4.false_list;
            c.BackpatchStream($8.next_list, $2.ip);
            c.BackpatchStream($9.next_list, $2.ip);

            c.BackpatchScope(ScopeType::Continue, $2.ip);
            c.BackpatchScope(ScopeType::Break, $10.ip);
        }
    | FOR '(' assignment_with_declaration ';' marker assignment ';' continue_marker assignment jump_marker ')' marker break_marker unmatched_statement jump_marker marker
        {
            LogDebug("P: Processing unmatched for");

            CheckIsInt($3, "Integer assignment is required in the first part of \"for\" statement", @3);
            CheckIsBool($6, "Bool expression is required in the middle part of \"for\" statement", @6);
            CheckIsInt($9, "Integer assignment is required in the last part of \"for\" statement", @9);

            CheckIsIfCompatible($6, "Unsupported expression for \"for\" statement", @6);

            c.BackpatchStream($3.true_list, $5.ip);
            c.BackpatchStream($14.next_list, $8.ip);
            c.BackpatchStream($15.next_list, $8.ip);
            $$.next_list = $6.false_list;
            c.BackpatchStream($6.true_list, $12.ip);
            c.BackpatchStream($9.true_list, $5.ip);
            c.BackpatchStream($10.next_list, $5.ip);

            c.BackpatchScope(ScopeType::Continue, $8.ip);
            c.BackpatchScope(ScopeType::Break, $16.ip);
        }

    | IF '(' assignment ')' marker matched_statement jump_marker ELSE marker unmatched_statement
        {
            LogDebug("P: Processing unmatched if..else");

            CheckIsIntOrBool($3, "Only integer and bool types are allowed in \"if\" statement", @3);

            CheckIsIfCompatible($3, "Unsupported expression for \"if\" statement", @3);

            c.BackpatchStream($3.true_list, $5.ip);
            c.BackpatchStream($3.false_list, $9.ip);
            $$.next_list = MergeLists($7.next_list, $10.next_list);
            $$.next_list = MergeLists($$.next_list, $6.next_list);
        }
    ;

switch_statement
    : case_list
        {
            $$.next_list = $1.next_list;
        }
    | case_list default_statement
        {
            $$.next_list = MergeLists($1.next_list, $2.next_list);
        }
    | default_statement case_list
        {
            $$.next_list = MergeLists($1.next_list, $2.next_list);
        }
    | case_list default_statement case_list
        {
            $$.next_list = MergeLists($1.next_list, $2.next_list);
            $$.next_list = MergeLists($$.next_list, $3.next_list);
        }
    ;

default_statement
    : DEFAULT ':' marker statement_list
        {
            SwitchBackpatchList* b = new SwitchBackpatchList();
            b->source_ip = $3.ip;
            b->is_default = true;
            b->line = @1.first_line;
            $$.next_list = b;
        }
    ;

case_list
    : CASE CONSTANT ':' marker statement_list
        {
            CheckIsConstant($2, @2);

            SwitchBackpatchList* b = new SwitchBackpatchList();
            b->source_ip = $4.ip;
            b->value = $2.value;
            b->type = $2.type;
            b->line = @2.first_line;
            $$.next_list = b;
        }
    | case_list CASE CONSTANT ':' marker statement_list
        {
            CheckIsConstant($3, @3);

            SwitchBackpatchList* prev = $1.next_list;
            while (prev) {
                if (strcmp($3.value, prev->value) == 0) {
                    std::string message = "Switch case \"";
                    message += $3.value;
                    message += "\" was already defined at line ";
                    message += std::to_string(prev->line);
                    throw CompilerException(CompilerExceptionSource::Statement,
                        message, @3.first_line, @3.first_column);
                }

                prev = prev->next;
            }
            
            SwitchBackpatchList* b = new SwitchBackpatchList();
            b->source_ip = $5.ip;
            b->value = $3.value;
            b->type = $3.type;
            b->line = @3.first_line;
            $$.next_list = MergeLists($1.next_list, b);
        }
    ;
   
// Variable declaration, without assignment
declaration_list
    : declaration ';'
        {
            LogDebug("P: Found declaration");
        }
    ;

declaration
    : declaration_type id
        {
            $$.type = $1;
            $$.size = 0;
            c.ToDeclarationList($1, 0, $2, ExpressionType::Variable);

            LogDebug("P: Found variable declaration");
        }
    | declaration_type '<' CONSTANT '>' id
        {
            CheckIsInt($3, "Array declaration must contain size of integer type", @3);
            CheckTypeIsPointerCompatible($1, "Specified type cannot be used as array type", @1);

            int32_t size = atoi($3.value);
            if (size < 1 || size > UINT16_MAX) {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "Specified array size is out of bounds", @3.first_line, @3.first_column);
            }

            $1.pointer++;

            $$.type = $1;
            $$.size = size;
            c.ToDeclarationList($1, size, $5, ExpressionType::Variable);

            LogDebug("P: Found array variable declaration");
        }
    | declaration ',' id
        {
            $$ = $1;
            c.ToDeclarationList($1.type, $1.size, $3, ExpressionType::Variable);

            LogDebug("P: Found multiple declarations");
        }
    ;

// Static variable declaration, without assignment
static_declaration_list
    : static_declaration ';'
        {
            LogDebug("P: Found static declaration");
        }
    ;

static_declaration
    : STATIC declaration_type id
        {
            $$.type = $2;
            $$.size = 0;
            c.AddStaticVariable($2, 0, $3);

            LogDebug("P: Found static variable declaration");
        }
    | STATIC declaration_type '<' CONSTANT '>' id
        {
            CheckIsInt($4, "Array declaration must contain size of integer type", @4);
            CheckTypeIsPointerCompatible($2, "Specified type cannot be used as array type", @2);

            int32_t size = atoi($4.value);
            if (size < 1 || size > UINT16_MAX) {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "Specified array size is out of bounds", @4.first_line, @4.first_column);
            }

            $2.pointer++;

            $$.type = $2;
            $$.size = size;
            c.AddStaticVariable($2, size, $6);

            LogDebug("P: Found static array variable declaration");
        }
    | static_declaration ',' id
        {
            $$ = $1;
            c.AddStaticVariable($1.type, $1.size, $3);

            LogDebug("P: Found multiple static declarations");
        }
    ;

// Variable assignment, without type declaration
assignment
    : expression
        {
            LogDebug("P: Found expression as assignment " << ($1.value ? $1.value : "???"));

            $$ = $1;
        }
    | id '=' assign_marker assignment
        {
            LogDebug("P: Found assignment");

            SymbolTableEntry* decl = c.GetParameter($1);
            if (!decl) {
                std::string message = "Variable \"";
                message += $1;
                message += "\" is not declared in scope";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            if (decl->exp_type == ExpressionType::Constant) {
                std::string message = "Variable \"";
                message += $1;
                message += "\" is read-only";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            if (!c.CanImplicitCast(decl->type, $4.type, $4.exp_type)) {
                std::string message = "Cannot assign to variable \"";
                message += $1;
                message += "\" because of type mismatch";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            PreAssign($4);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            i->assignment.type = AssignType::None;
            i->assignment.dst_value = decl->name;
            CopyOperand(i->assignment.op1, $4);

            $$.value = $1;
            $$.type = decl->type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;

            PostAssign($$, $4);
        }
    | id '[' expression ']' '=' assign_marker assignment
        {
            LogDebug("P: Found array assignment");

            SymbolTableEntry* decl = c.GetParameter($1);
            if (!decl) {
                std::string message = "Variable \"";
                message += $1;
                message += "\" is not declared in scope";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            if (decl->exp_type == ExpressionType::Constant) {
                std::string message = "Variable \"";
                message += $1;
                message += "\" is read-only";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            if (decl->type.pointer == 0) {
                std::string message = "Variable \"";
                message += $1;
                message += "\" is not declared as pointer";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            SymbolType resolved_type = decl->type;
            resolved_type.pointer--;
            if (!c.CanImplicitCast(resolved_type, $7.type, $7.exp_type)) {
                std::string message = "Cannot assign to variable \"";
                message += $1;
                message += "\" because of type mismatch";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            CheckIsInt($3, "Only integer types are allowed as array index", @3);
            
            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($3);

            PreAssign($7);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            i->assignment.type = AssignType::None;
            i->assignment.dst_value = decl->name;
            i->assignment.dst_index.value = $3.value;
            i->assignment.dst_index.type = $3.type;
            i->assignment.dst_index.exp_type = $3.exp_type;
            CopyOperand(i->assignment.op1, $7);

            //$$.true_list = $7.true_list;

            $$.value = $1;
            $$.type = decl->type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = $3.value;
            $$.index.type = $3.type;
            $$.index.exp_type = $3.exp_type;

            PostAssign($$, $7);
        }
    ;

// Variable assignment, could be with or without type declaration
assignment_with_declaration
    : assignment
        {
            LogDebug("P: Found assignment without declaration \"" << ($1.value ? $1.value : "???") << "\"");

            $$ = $1;
        }
    | CONST declaration_type id '=' expression
        {
            LogDebug("P: Found const. variable declaration with assignment \"" << $3 << "\"");

            if ($5.exp_type != ExpressionType::Constant) {
                std::string message = "Cannot assign non-constant value to variable \"";
                message += $3;
                message += "\"";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            if (!c.CanImplicitCast($2, $5.type, $5.exp_type)) {
                std::string message = "Cannot assign to variable \"";
                message += $3;
                message += "\" because of type mismatch";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            SymbolTableEntry* decl = c.ToDeclarationList($2, 0, $3, ExpressionType::Constant);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            i->assignment.type = AssignType::None;
            i->assignment.dst_value = decl->name;
            CopyOperand(i->assignment.op1, $5);

            $$.value = $3;
            $$.type = $2;
            $$.exp_type = ExpressionType::Constant;
            $$.index.value = nullptr;
        }
    | declaration_type id '=' assign_marker expression
        {
            LogDebug("P: Found variable declaration with assignment \"" << $2 << "\"");

            if (!c.CanImplicitCast($1, $5.type, $5.exp_type)) {
                std::string message = "Cannot assign to variable \"";
                message += $2;
                message += "\" because of type mismatch";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            PreAssign($5);

            SymbolTableEntry* decl = c.ToDeclarationList($1, 0, $2, ExpressionType::None);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            i->assignment.type = AssignType::None;
            i->assignment.dst_value = decl->name;
            CopyOperand(i->assignment.op1, $5);

            $$.value = $2;
            $$.type = $1;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;

            PostAssign($$, $5);
        }
    ;

expression
    : INC_OP expression
        {
            LogDebug("P: Processing increment");

            CheckIsInt($2, "Specified type is not allowed in arithmetic operations", @1);

            // Create a variable if needed
            SymbolTableEntry* decl;
            if ($2.exp_type != ExpressionType::Variable) {
                decl = c.GetUnusedVariable($2.type);

                InstructionEntry* i = c.AddToStream(InstructionType::Assign);
                i->assignment.dst_value = decl->name;
                CopyOperand(i->assignment.op1, $2);

                $2.value = decl->name;
                $2.type = decl->type;
                $2.exp_type = ExpressionType::Variable;
            } else {
                decl = nullptr;
            }

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            i->assignment.type = AssignType::Add;
            if (decl) {
                i->assignment.dst_value = decl->name;
            } else {
                i->assignment.dst_value = $2.value;
                i->assignment.dst_index.value = $2.index.value;
                i->assignment.dst_index.type = $2.index.type;
                i->assignment.dst_index.exp_type = $2.index.exp_type;
            }
            CopyOperand(i->assignment.op1, $2);

            i->assignment.op2.value = _strdup("1");
            i->assignment.op2.type = $2.type;
            i->assignment.op2.exp_type = ExpressionType::Constant;

            $$ = $2;
            $$.true_list = nullptr;
            $$.false_list = nullptr;
        }
    | DEC_OP expression
        {
            LogDebug("P: Processing decrement");

            CheckIsInt($2, "Specified type is not allowed in arithmetic operations", @1);

            // Create a variable if needed
            SymbolTableEntry* decl;
            if ($2.exp_type != ExpressionType::Variable) {
                decl = c.GetUnusedVariable($2.type);

                InstructionEntry* i = c.AddToStream(InstructionType::Assign);
                i->assignment.dst_value = decl->name;
                CopyOperand(i->assignment.op1, $2);

                $2.value = decl->name;
                $2.type = decl->type;
                $2.exp_type = ExpressionType::Variable;
            } else {
                decl = nullptr;
            }

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            i->assignment.type = AssignType::Subtract;
            if (decl) {
                i->assignment.dst_value = decl->name;
            } else {
                i->assignment.dst_value = $2.value;
                i->assignment.dst_index.value = $2.index.value;
                i->assignment.dst_index.type = $2.index.type;
                i->assignment.dst_index.exp_type = $2.index.exp_type;
            }
            CopyOperand(i->assignment.op1, $2);

            i->assignment.op2.value = _strdup("1");
            i->assignment.op2.type = $2.type;
            i->assignment.op2.exp_type = ExpressionType::Constant;

            $$ = $2;
            $$.true_list = nullptr;
            $$.false_list = nullptr;
        }
    | expression LOG_OR marker expression
        {
            CheckIsIntOrBool($1, "Specified type is not allowed in logical operations", @1);
            CheckIsIntOrBool($4, "Specified type is not allowed in logical operations", @4);

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeededMarker($1, $3);
            PrepareIndexedVariableIfNeededMarker($4, $3);
            
            PreIfMarker($3);
            PrepareExpressionsForLogical($1, $3, $4);

            $$.true_list = MergeLists($1.true_list, $4.true_list);
            c.BackpatchStream($1.false_list, $3.ip);
            $$.false_list = $4.false_list;
            
            PostIf($$, $1);
        }
    | expression LOG_AND marker expression
        {
            CheckIsIntOrBool($1, "Specified type is not allowed in logical operations", @1);
            CheckIsIntOrBool($4, "Specified type is not allowed in logical operations", @4);

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeededMarker($1, $3);
            PrepareIndexedVariableIfNeededMarker($4, $3);
            
            PreIfMarker($3);
            PrepareExpressionsForLogical($1, $3, $4);

            $$.false_list = MergeLists($1.false_list, $4.false_list);
            c.BackpatchStream($1.true_list, $3.ip);
            $$.true_list = $4.true_list;
            
            PostIf($$, $1);
        }
    | expression NOT_EQUAL expression
        {
            LogDebug("P: Processing logical not equal");

            if ($1.type.base != BaseSymbolType::String || $3.type.base != BaseSymbolType::String) {
                CheckIsIntOrBool($1, "Only integer and bool types are allowed in comparsions", @1);
                CheckIsIntOrBool($3, "Only integer and bool types are allowed in comparsions", @3);
            }

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            PreIf();

            CreateIfWithBackpatch($$.true_list, CompareType::NotEqual, $1, $3);
            $$.false_list = c.AddToStreamWithBackpatch(InstructionType::Goto);

            PostIf($$, $1);
        }
    | expression EQUAL expression
        {
            LogDebug("P: Processing logical equal");

            if ($1.type.base != BaseSymbolType::String || $3.type.base != BaseSymbolType::String) {
                CheckIsIntOrBool($1, "Only integer and bool types are allowed in comparsions", @1);
                CheckIsIntOrBool($3, "Only integer and bool types are allowed in comparsions", @3);
            }

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            PreIf();

            CreateIfWithBackpatch($$.true_list, CompareType::Equal, $1, $3);
            $$.false_list = c.AddToStreamWithBackpatch(InstructionType::Goto);

            if ($1.type.base == BaseSymbolType::Bool) {
                $$.true_list = MergeLists($$.true_list, $1.true_list);
                $$.false_list = MergeLists($$.false_list, $1.false_list);
            }
            if ($3.type.base == BaseSymbolType::Bool) {
                $$.true_list = MergeLists($$.true_list, $3.true_list);
                $$.false_list = MergeLists($$.false_list, $3.false_list);
            }
            
            PostIf($$, $1);
        }
    | expression GREATER_OR_EQUAL expression
        {
            LogDebug("P: Processing logical greater or equal");

            CheckIsInt($1, "Only integer types are allowed in comparsions", @1);
            CheckIsInt($3, "Only integer types are allowed in comparsions", @3);

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            PreIf();

            CreateIfWithBackpatch($$.true_list, CompareType::GreaterOrEqual, $1, $3);
            $$.false_list = c.AddToStreamWithBackpatch(InstructionType::Goto);

            PostIf($$, $1);
        }
    | expression LESS_OR_EQUAL expression
        {
            LogDebug("P: Processing logical smaller or equal");

            CheckIsInt($1, "Only integer types are allowed in comparsions", @1);
            CheckIsInt($3, "Only integer types are allowed in comparsions", @3);

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            PreIf();

            CreateIfWithBackpatch($$.true_list, CompareType::LessOrEqual, $1, $3);
            $$.false_list = c.AddToStreamWithBackpatch(InstructionType::Goto);

            PostIf($$, $1);
        }
    | expression '>' expression
        {
            LogDebug("P: Processing logical bigger");

            CheckIsInt($1, "Only integer types are allowed in comparsions", @1);
            CheckIsInt($3, "Only integer types are allowed in comparsions", @3);

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            PreIf();

            CreateIfWithBackpatch($$.true_list, CompareType::Greater, $1, $3);
            $$.false_list = c.AddToStreamWithBackpatch(InstructionType::Goto);

            PostIf($$, $1);
        }
    | expression '<' expression
        {
            LogDebug("P: Processing logical smaller");

            CheckIsInt($1, "Only integer types are allowed in comparsions", @1);
            CheckIsInt($3, "Only integer types are allowed in comparsions", @3);

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            PreIf();

            CreateIfWithBackpatch($$.true_list, CompareType::Less, $1, $3);
            $$.false_list = c.AddToStreamWithBackpatch(InstructionType::Goto);

            PostIf($$, $1);
        }
    | expression SHIFT_LEFT expression
        {
            LogDebug("P: Processing left shift");

            CheckIsInt($1, "Only integer types are allowed in shift operations", @1);
            CheckIsInt($3, "Only integer types are allowed in shift operations", @3);

            CheckIsNotPointer($1, "Pointers are not allowed in shift operations", @1);
            CheckIsNotPointer($3, "Pointers are not allowed in shift operations", @3);

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            SymbolTableEntry* decl = c.GetUnusedVariable($1.type);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            FillInstructionForAssign(i, AssignType::ShiftLeft, decl, $1, $3);

            $$.value = decl->name;
            $$.type = $1.type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;
            $$.true_list = nullptr;
            $$.false_list = nullptr;
        }
    | expression SHIFT_RIGHT expression
        {
            LogDebug("P: Processing left shift");

            CheckIsInt($1, "Only integer types are allowed in shift operations", @1);
            CheckIsInt($3, "Only integer types are allowed in shift operations", @3);

            CheckIsNotPointer($1, "Pointers are not allowed in shift operations", @1);
            CheckIsNotPointer($3, "Pointers are not allowed in shift operations", @3);
        
            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            SymbolTableEntry* decl = c.GetUnusedVariable($1.type);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            FillInstructionForAssign(i, AssignType::ShiftRight, decl, $1, $3);

            $$.value = decl->name;
            $$.type = $1.type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;
            $$.true_list = nullptr;
            $$.false_list = nullptr;
        }
    | expression '+' expression
        {
            if ($1.type.base == BaseSymbolType::String && $3.type.base == BaseSymbolType::String) {
                LogDebug("P: Processing string concatenation");
            
                SymbolTableEntry* decl = c.GetUnusedVariable({ BaseSymbolType::String, 0 });

                InstructionEntry* i = c.AddToStream(InstructionType::Assign);
                FillInstructionForAssign(i, AssignType::Add, decl, $1, $3);

                $$.value = decl->name;
                $$.type = { BaseSymbolType::String, 0 };
                $$.exp_type = ExpressionType::Variable;
                $$.index.value = nullptr;
                $$.true_list = nullptr;
                $$.false_list = nullptr;
            } else {
                LogDebug("P: Processing addition");

                SymbolType type;
                if ($1.type.pointer > 0 || $3.type.pointer > 0) {
                    // Pointer arithmetic
                    if ($1.type.pointer > 0 && $3.type.pointer > 0) {
                        throw CompilerException(CompilerExceptionSource::Statement,
                            "Cannot use two pointer operands in this context", @1.first_line, @1.first_column);
                    }

                    if ($1.type.pointer > 0) {
                        type = $1.type;
                    } else {
                        type = $3.type;
                    }
                } else {
                    // Standard arithmetic
                    type = c.GetLargestTypeForArithmetic($1.type, $3.type);
                    if (type.base == BaseSymbolType::Unknown) {
                        throw CompilerException(CompilerExceptionSource::Statement,
                            "Specified type is not allowed in arithmetic operations", @1.first_line, @1.first_column);
                    }
                }

                // Move indexed variables to temp. variables
                PrepareIndexedVariableIfNeeded($1);
                PrepareIndexedVariableIfNeeded($3);

                SymbolTableEntry* decl = c.GetUnusedVariable(type);

                InstructionEntry* i = c.AddToStream(InstructionType::Assign);
                FillInstructionForAssign(i, AssignType::Add, decl, $1, $3);

                $$.value = decl->name;
                $$.type = type;
                $$.exp_type = ExpressionType::Variable;
                $$.index.value = nullptr;
                $$.true_list = nullptr;
                $$.false_list = nullptr;
            }
        }
    | expression '-' expression
        {
            LogDebug("P: Processing substraction");

            SymbolType type;
            if ($1.type.pointer > 0 || $3.type.pointer > 0) {
                // Pointer arithmetic
                if ($1.type.pointer > 0 && $3.type.pointer > 0) {
                    throw CompilerException(CompilerExceptionSource::Statement,
                        "Cannot use two pointer operands in this context", @1.first_line, @1.first_column);
                }

                if ($1.type.pointer > 0) {
                    type = $1.type;
                } else {
                    type = $3.type;
                }
            } else {
                // Standard arithmetic
                type = c.GetLargestTypeForArithmetic($1.type, $3.type);
                if (type.base == BaseSymbolType::Unknown) {
                    throw CompilerException(CompilerExceptionSource::Statement,
                        "Specified type is not allowed in arithmetic operations", @1.first_line, @1.first_column);
                }
            }

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            SymbolTableEntry* decl = c.GetUnusedVariable(type);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            FillInstructionForAssign(i, AssignType::Subtract, decl, $1, $3);

            $$.value = decl->name;
            $$.type = type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;
            $$.true_list = nullptr;
            $$.false_list = nullptr;
        }
    | expression '*' expression
        {
            LogDebug("P: Processing multiplication");

            CheckIsNotPointer($1, "Pointers are not allowed in this context", @1);
            CheckIsNotPointer($3, "Pointers are not allowed in this context", @3);

            SymbolType type = c.GetLargestTypeForArithmetic($1.type, $3.type);
            if (type.base == BaseSymbolType::Unknown) {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "Specified type is not allowed in arithmetic operations", @1.first_line, @1.first_column);
            }

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            SymbolTableEntry* decl = c.GetUnusedVariable(type);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            FillInstructionForAssign(i, AssignType::Multiply, decl, $1, $3);

            $$.value = decl->name;
            $$.type = type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;
            $$.true_list = nullptr;
            $$.false_list = nullptr;
        }
    | expression '/' expression
        {
            LogDebug("P: Processing division");

            CheckIsNotPointer($1, "Pointers are not allowed in this context", @1);
            CheckIsNotPointer($3, "Pointers are not allowed in this context", @3);

            SymbolType type = c.GetLargestTypeForArithmetic($1.type, $3.type);
            if (type.base == BaseSymbolType::Unknown) {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "Specified type is not allowed in arithmetic operations", @1.first_line, @1.first_column);
            }

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            SymbolTableEntry* decl = c.GetUnusedVariable(type);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            FillInstructionForAssign(i, AssignType::Divide, decl, $1, $3);

            $$.value = decl->name;
            $$.type = type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;
            $$.true_list = nullptr;
            $$.false_list = nullptr;
        }
    | expression '%' expression
        {
            LogDebug("P: Processing remainder");

            CheckIsNotPointer($1, "Pointers are not allowed in this context", @1);
            CheckIsNotPointer($3, "Pointers are not allowed in this context", @3);

            SymbolType type = c.GetLargestTypeForArithmetic($1.type, $3.type);
            if (type.base == BaseSymbolType::Unknown) {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "Specified type is not allowed in arithmetic operations", @1.first_line, @1.first_column);
            }

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($1);
            PrepareIndexedVariableIfNeeded($3);

            SymbolTableEntry* decl = c.GetUnusedVariable(type);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            FillInstructionForAssign(i, AssignType::Remainder, decl, $1, $3);

            $$.value = decl->name;
            $$.type = type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;
            $$.true_list = nullptr;
            $$.false_list = nullptr;
        }
    | '!' expression
        {
            LogDebug("P: Processing logical not");

            if ($2.type.base == BaseSymbolType::Bool && $2.type.pointer == 0) {
                $$ = $2;
                $$.true_list = $2.false_list;
                $$.false_list = $2.true_list;
            } else if ($2.type.base == BaseSymbolType::Uint8 || $2.type.base == BaseSymbolType::Uint16 || $2.type.base == BaseSymbolType::Uint32) {
                CreateIfConstWithBackpatch($$.false_list, CompareType::NotEqual, $2, "0");

                $$.true_list = c.AddToStreamWithBackpatch(InstructionType::Goto);
            } else {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "Specified type is not allowed in logical operations", @1.first_line, @1.first_column);
            }
        }
    | U_PLUS expression
        {
            CheckIsInt($2, "Unary operator is not allowed in this context", @1);

            $$ = $2;
        }
    | U_MINUS expression
        {
            CheckIsInt($2, "Unary operator is not allowed in this context", @1);

            SymbolTableEntry* decl = c.GetUnusedVariable($2.type);
            decl->exp_type = $2.exp_type;

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            i->assignment.type = AssignType::Negation;
            i->assignment.dst_value = decl->name;
            CopyOperand(i->assignment.op1, $2);

            $$ = $2;
            $$.value = decl->name;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;
       }
    | CONSTANT
        {
            LogDebug("P: Processing constant");

            $$.value = _strdup($1.value);
            $$.type = $1.type;
            $$.exp_type = ExpressionType::Constant;
            $$.index.value = nullptr;
            $$.true_list = nullptr;
            $$.false_list = nullptr;
        }
    | '(' expression ')'
        {
            LogDebug("P: Processing expression in parentheses");

            $$ = $2;
        }
    | CAST '<' declaration_type '>' '(' expression ')'
        {
            LogDebug("P: Processing explicit cast");

            if (!c.IsScopeActive(ScopeType::Assign)) {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "Explicit cast cannot be used in this context", @1.first_line, @1.first_column);
            }

            if (!c.CanExplicitCast($3, $6.type)) {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "This explicit cast type cannot be used in this context", @1.first_line, @1.first_column);
            }

            $$ = $6;
            $$.type = $3;
        }
    | ALLOC '<' declaration_type '>' '(' expression ')'
        {
            LogDebug("P: Processing alloc");

            if (!c.IsScopeActive(ScopeType::Assign)) {
                throw CompilerException(CompilerExceptionSource::Statement,
                    "Allocation cannot be used in this context", @1.first_line, @1.first_column);
            }

            CheckTypeIsPointerCompatible($3, "Specified type cannot be used for allocation", @3);
            CheckIsInt($6, "Only integer types are allowed to specify memory block size", @6);

            SymbolTableEntry* func = c.GetFunction("#Alloc");
            if (!func) {
                ThrowOnUnreachableCode();
            }

            SymbolType ptr_type = $3;
            ptr_type.pointer++;

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($6);

            SymbolTableEntry* decl = c.GetUnusedVariable(ptr_type);

            uint8_t shift = c.SizeToShift(c.GetSymbolTypeSize($3));

            SymbolTableEntry* param_copy = new SymbolTableEntry();
            if (shift == 0) {
                param_copy->name = _strdup($6.value);
                param_copy->type = $6.type;
                param_copy->exp_type = $6.exp_type;
            } else {
                SymbolTableEntry* param = c.GetUnusedVariable({ BaseSymbolType::Uint32, 0 });

                InstructionEntry* i1 = c.AddToStream(InstructionType::Assign);
                i1->assignment.type = AssignType::ShiftLeft;
                i1->assignment.dst_value = param->name;
                CopyOperand(i1->assignment.op1, $6);
                i1->assignment.op2.value = _strdup(std::to_string(shift).c_str());
                i1->assignment.op2.type = { BaseSymbolType::Uint8, 0 };
                i1->assignment.op2.exp_type = ExpressionType::Constant;
                i1->assignment.op2.index.value = nullptr;

                param_copy->name = _strdup(param->name);
                param_copy->type = param->type;
                param_copy->exp_type = param->exp_type;
            }

            $$.value = decl->name;
            $$.type = decl->type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;
            c.PrepareForCall(func->name, param_copy, 1);

            InstructionEntry* i2 = c.AddToStream(InstructionType::Call);
            i2->call_statement.target = func;
            i2->call_statement.return_symbol = decl->name;
        }
    | id '(' call_parameter_list ')'
        {
            LogDebug("P: Processing function call with parameters");

            SymbolTableEntry* func = c.GetFunction($1);
            if (!func || func->return_type.base == BaseSymbolType::Unknown) {
                std::string message = "Function \"";
                message += $1;
                message += "\" is not defined";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            if (func->return_type.base == BaseSymbolType::Void && func->return_type.pointer == 0) {
                // Has void return
                $$.type = { BaseSymbolType::None, 0 };
                $$.value = nullptr;
                $$.exp_type = ExpressionType::None;
                $$.index.value = nullptr;
                c.PrepareForCall(func->name, $3.list, $3.count);

                InstructionEntry* i = c.AddToStream(InstructionType::Call);
                i->call_statement.target = func;
            } else {
                // Has return value
                $$.type = func->return_type;

                SymbolTableEntry* decl = c.GetUnusedVariable($$.type);

                $$.value = decl->name;
                $$.exp_type = ExpressionType::Variable;
                $$.index.value = nullptr;
                c.PrepareForCall(func->name, $3.list, $3.count);

                InstructionEntry* i = c.AddToStream(InstructionType::Call);
                i->call_statement.target = func;
                i->call_statement.return_symbol = decl->name;
            }
        }
    | id '('  ')'
        {
            LogDebug("P: Processing function call");

            SymbolTableEntry* func = c.GetFunction($1);
            if (!func || func->return_type.base == BaseSymbolType::Unknown) {
                std::string message = "Function \"";
                message += $1;
                message += "\" is not defined";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            if (func->return_type.base == BaseSymbolType::Void && func->return_type.pointer == 0) {
                // Has return value
                $$.type = { BaseSymbolType::None, 0 };
                $$.value = nullptr;
                $$.exp_type = ExpressionType::None;
                $$.index.value = nullptr;
                c.PrepareForCall(func->name, nullptr, 0);

                InstructionEntry* i = c.AddToStream(InstructionType::Call);
                i->call_statement.target = func;
            } else {
                // Has return value
                $$.type = func->return_type;

                SymbolTableEntry* decl = c.GetUnusedVariable($$.type);

                $$.value = decl->name;
                $$.exp_type = ExpressionType::Variable;
                $$.index.value = nullptr;
                c.PrepareForCall(func->name, nullptr, 0);

                InstructionEntry* i = c.AddToStream(InstructionType::Call);
                i->call_statement.target = func;
                i->call_statement.return_symbol = decl->name;
            }
        }
    | '&' id
        {
            LogDebug("P: Processing reference");

            SymbolTableEntry* param = c.GetParameter($2);
            if (!param) {
                std::string message = "Variable \"";
                message += $2;
                message += "\" is not declared in scope";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            SymbolType reference_type = param->type;
            reference_type.pointer++;
            SymbolTableEntry* decl = c.GetUnusedVariable(reference_type);

            InstructionEntry* i = c.AddToStream(InstructionType::Assign);
            i->assignment.dst_value = decl->name;
            i->assignment.op1.value = param->name;
            i->assignment.op1.type = param->type;
            i->assignment.op1.exp_type = ExpressionType::Variable;
            i->assignment.op1.index.value = nullptr;

            $$.value = decl->name;
            $$.type = decl->type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;
        }
    | id '[' expression ']'
        {
            LogDebug("P: Processing array identifier");

            SymbolTableEntry* param = c.GetParameter($1);
            if (!param) {
                std::string message = "Variable \"";
                message += $1;
                message += "\" is not declared in scope";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            if (param->type.pointer == 0) {
                std::string message = "Variable \"";
                message += $1;
                message += "\" is not declared as pointer";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            CheckIsInt($3, "Only integer types are allowed as array index", @3);

            SymbolType resolved_type = param->type;
            resolved_type.pointer--;

            $$.value = $1;
            $$.type = resolved_type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = $3.value;
            $$.index.type = $3.type;
            $$.index.exp_type = $3.exp_type;
        }
    | id
        {
            LogDebug("P: Processing identifier");

            SymbolTableEntry* param = c.GetParameter($1);
            if (!param) {
                std::string message = "Variable \"";
                message += $1;
                message += "\" is not declared in scope";
                throw CompilerException(CompilerExceptionSource::Statement,
                    message, @1.first_line, @1.first_column);
            }

            $$.value = $1;
            $$.type = param->type;
            $$.exp_type = ExpressionType::Variable;
            $$.index.value = nullptr;
        }
    ;

call_parameter_list
    : assign_marker expression
        {
            LogDebug("P: Processing call parameter list");

            CheckTypeIsValid($2.type, @2);

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($2);

            PreCallParam($2);

            $$.list = c.ToCallParameterList(nullptr, $2.type, $2.value, $2.exp_type);
            $$.count = 1;

            PostCallParam($2);
        }
    | call_parameter_list ',' assign_marker expression
        {
            LogDebug("P: Processing call parameter list");

            CheckTypeIsValid($4.type, @4);

            // Move indexed variables to temp. variables
            PrepareIndexedVariableIfNeeded($4);

            PreCallParam($4);

            $$.list = c.ToCallParameterList($1.list, $4.type, $4.value, $4.exp_type);
            $$.count = $1.count + 1;

            PostCallParam($4);
        }
    ;

// Goto
// ToDo: Add beginning-of-line only marker?
goto_label
    : id ':' marker
        {
            LogDebug("P: Found goto label \"" << $1 << "\" (" << $3.ip << ")");

            c.AddLabel($1, $3.ip);
        }
    ;

// Misc.
id
    : IDENTIFIER
        {
            LogDebug("P: Found identifier \"" << $1 << "\"");

            $$ = _strdup(yytext);
        }
    ;

marker
    :   {    
            LogDebug("P: Generating marker");

            $$.ip = c.NextIp();
        }
    ;

jump_marker
    :   {
            LogDebug("P: Generating jump marker");

            $$.ip = c.NextIp();
            $$.next_list = c.AddToStreamWithBackpatch(InstructionType::Goto);
        }
    ;

assign_marker
    :	{
            LogDebug("P: Generating assign marker");

            c.IncreaseScope(ScopeType::Assign);
        }
    ;

break_marker
    :   {
            LogDebug("P: Generating break marker");

            c.IncreaseScope(ScopeType::Break);
        }
    ;

continue_marker
    :   {
            LogDebug("P: Generating continue marker");

            $$.ip = c.NextIp();
            c.IncreaseScope(ScopeType::Continue);
        }
    ;

switch_next
    :   {
            LogDebug("P: Generating switch next marker");

            $$.next_list = c.AddToStreamWithBackpatch(InstructionType::Goto);
            $$.ip = c.NextIp();
        }
    ;

%%

void yyerror(const char* s)
{
    if (memcmp(s, "syntax error", 12) == 0) {
        if (memcmp(s + 12, ", ", 2) == 0) {
            throw CompilerException(CompilerExceptionSource::Syntax,
                s + 14, yylloc.first_line, yylloc.first_column);
        }

        throw CompilerException(CompilerExceptionSource::Syntax,
            s + 12, yylloc.first_line, yylloc.first_column);
    }

    throw CompilerException(CompilerExceptionSource::Syntax,
        s, yylloc.first_line, yylloc.first_column);
}