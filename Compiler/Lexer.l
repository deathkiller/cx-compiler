%option noyywrap
%option yylineno
%option case-insensitive
%option nostdinit
%option stack

%x STATE_STRING
%x STATE_CHAR

%{

// Fix MSVC warnings
#pragma warning (push)
#pragma warning (disable : 4005)
#include <stdint.h>
#pragma warning (pop)

#include <stdio.h>
#include <stdbool.h>

#include "Log.h"
#include "Compiler.h"
#include "parser.tab.h"

#define YY_DECL int yylex()
#define YY_USER_ACTION                                  \
    yylloc.first_line = yylloc.last_line = yylineno;    \
    yylloc.first_column = yycolumn;                     \
    yylloc.last_column = yycolumn + yyleng - 1;         \
    yycolumn += yyleng;

int yycolumn = 1;
bool allow_unary = false;

char string_buffer[32768];
char* string_buffer_ptr;

extern Compiler c;

%}

NEWLINE (\n|\r\n)
WHITESPACE (" "|\t)

LINE_COMMENT ("//".*{NEWLINE})
BLOCK_COMMENT (\/\*([^*]|[\r\n]|(\*+([^*/]|[\r\n])))*\*+\/)

DIRECTIVE ("#".*{NEWLINE})

INTEGER (0|[1-9][0-9]*)
BOOL_TRUE true
BOOL_FALSE false
NULL null
IDENTIFIER ([a-zA-Z_][a-zA-Z0-9_]*)

%%

<<EOF>> {
    FILE* yyin_old = yyin;

    yypop_buffer_state();

    if (yyin_old != yyin && yyin_old != stdin) {
        fclose(yyin_old);
    }

    if (!YY_CURRENT_BUFFER) {
        yyterminate();
    }
}

({WHITESPACE}+) {
    // Ignore whitespaces
}

({NEWLINE}+) {
    // Ignore newlines
    yycolumn = 1;
}

({LINE_COMMENT}|{BLOCK_COMMENT}) {
    // Ignore comments
}

{DIRECTIVE} {
    c.ParseCompilerDirective(yytext, [&](char* directive, char* param) {
        LogDebug("L: Found preprocessor directive \"" << directive << "\"");

        if (param && strcmp(directive, "#include") == 0) {
            // Include file directive
            char* path_start = param;
            if (*path_start == '"') {
                path_start++;
            }

            char* path_end = path_start;
            while (*path_end && *path_end != '"') {
                path_end++;
            }

            char* path = new char[path_end - path_start + 1];
            memcpy(path, path_start, path_end - path_start);
            path[path_end - path_start] = '\0';

            errno_t err = fopen_s(&yyin, path, "rb");

            delete path;

            if (err) {
                throw CompilerException(CompilerExceptionSource::Unknown, "Cannot open include file");
            }

            yypush_buffer_state(yy_create_buffer(yyin, YY_BUF_SIZE));

            BEGIN(INITIAL);
            return true;
        }
    
        return false;
    });
}

^"-" {
    LogDebug("L: Found unary minus");

    allow_unary = false;
    return U_MINUS;
}

^"+" {
    LogDebug("L: Found unary plus");

    allow_unary = false;
    return U_PLUS;
}

("("|"{"|"["|"<"|">"|"="|";"|","|"!"|":") {
    LogDebug("L: Found " << yytext[0]);

    allow_unary = true;
    return yytext[0];
}

(")"|"}"|"]"|"&") {
    LogDebug("L: Found " << yytext[0]);

    allow_unary = false;
    return yytext[0];
}

("/"|"*"|"%") {
    LogDebug("L: Found " << yytext[0]);

    allow_unary = true;
    return yytext[0];
}

"-" {
    if (allow_unary) {
        LogDebug("L: Found unary minus");

        allow_unary = false;
        return U_MINUS;
    } else {
        LogDebug("L: Found minus");

        return yytext[0];
    }
}

"+" {
    if (allow_unary) {
        LogDebug("L: Found unary plus");

        allow_unary = false;
        return U_PLUS;
    } else {
        LogDebug("L: Found plus");

        return yytext[0];
    }
}

"const" {
    LogDebug("L: Found \"const\"");

    return CONST;
}

"static" {
    LogDebug("L: Found \"static\"");

    return STATIC;
}

"void" {
    LogDebug("L: Found \"void\"");

    return VOID;
}

"bool" {
    LogDebug("L: Found \"bool\"");

    return BOOL;
}

"uint8" {
    LogDebug("L: Found \"uint8\"");

    return UINT8;
}

"uint16" {
    LogDebug("L: Found \"uint16\"");

    return UINT16;
}

"uint32" {
    LogDebug("L: Found \"uint32\"");

    return UINT32;
}

"string" {
    LogDebug("L: Found \"string\"");

    return STRING;
}

"if" {
    LogDebug("L: Found \"if\"");

    return IF;
}

"else" {
    LogDebug("L: Found \"else\"");

    return ELSE;
}

"return" {
    LogDebug("L: Found \"return\"");

    return RETURN;
}

"do" {
    LogDebug("L: Found \"do\"");

    return DO;
}

"while" {
    LogDebug("L: Found \"while\"");

    return WHILE;
}

"for" {
    LogDebug("L: Found \"for\"");

    return FOR;
}

"switch" {
    LogDebug("L: Found \"switch\"");

    return SWITCH;
}

"case" {
    LogDebug("L: Found \"case\"");

    return CASE;
}

"default" {
    LogDebug("L: Found \"default\"");

    return DEFAULT;
}

"continue" {
    LogDebug("L: Found \"continue\"");

    return CONTINUE;
}

"break" {
    LogDebug("L: Found \"break\"");

    return BREAK;
}

"goto" {
    LogDebug("L: Found \"goto\"");

    return GOTO;
}

"cast" {
    LogDebug("L: Found \"cast\"");

    return CAST;
}

"alloc" {
    LogDebug("L: Found \"alloc\"");

    return ALLOC;
}

"++" {
    LogDebug("L: Found ++");

    return INC_OP;
}

"--" {
    LogDebug("L: Found --");

    return DEC_OP;
}

"==" {
    LogDebug("L: Found ==");

    return EQUAL;
}

"!=" {
    LogDebug("L: Found !=");

    return NOT_EQUAL;
}

">=" {
    LogDebug("L: Found >=");

    return GREATER_OR_EQUAL;
}

"<=" {
    LogDebug("L: Found <=");

    return LESS_OR_EQUAL;
}

"<<" {
    LogDebug("L: Found <<");

    return SHIFT_LEFT;
}

">>" {
    LogDebug("L: Found >>");

    return SHIFT_RIGHT;
}

"&&" {
    LogDebug("L: Found &&");

    return LOG_AND;
}

"||" {
    LogDebug("L: Found ||");

    return LOG_OR;
}

{INTEGER} {
    LogDebug("L: Found integer constant \"" << yytext << "\"");

    yylval.expression.value = _strdup(yytext);
    yylval.expression.exp_type = ExpressionType::Constant;

    int32_t value = atoi(yytext);
    if (value == (int8_t)value || value == (uint8_t)value) {
        yylval.expression.type = { BaseSymbolType::Uint8, 0 };
    } else if (value == (int16_t)value || value == (uint16_t)value) {
        yylval.expression.type = { BaseSymbolType::Uint16, 0 };
    } else {
        yylval.expression.type = { BaseSymbolType::Uint32, 0 };
    }

    allow_unary = false;
    return CONSTANT;
}

{BOOL_TRUE} {
    LogDebug("L: Found bool constant \"true\"");

    yylval.expression.value = _strdup("1");
    yylval.expression.exp_type = ExpressionType::Constant;
    yylval.expression.type = { BaseSymbolType::Bool, 0 };
    allow_unary = false;
    return CONSTANT;
}

{BOOL_FALSE} {
    LogDebug("L: Found bool constant \"false\"");

    yylval.expression.value = _strdup("0");
    yylval.expression.exp_type = ExpressionType::Constant;
    yylval.expression.type = { BaseSymbolType::Bool, 0 };
    allow_unary = false;
    return CONSTANT;
}

{NULL} {
    LogDebug("L: Found null");

    yylval.expression.value = _strdup("0");
    yylval.expression.exp_type = ExpressionType::Constant;
    yylval.expression.type = { BaseSymbolType::Void, 1 };
    allow_unary = false;
    return CONSTANT;
}

{IDENTIFIER} {
    LogDebug("L: Found identifier \"" << yytext << "\"");

    yylval.string = _strdup(yytext);
    allow_unary = false;
    return IDENTIFIER;
}

\" {
    string_buffer_ptr = string_buffer;
    BEGIN(STATE_STRING);
}

<STATE_STRING>{
    \" {
        BEGIN(INITIAL);
        *string_buffer_ptr = '\0';

        LogDebug("L: Found string constant \"" << string_buffer << "\"");

        yylval.expression.value = _strdup(string_buffer);
        yylval.expression.exp_type = ExpressionType::Constant;
        yylval.expression.type = { BaseSymbolType::String, 0 };
        allow_unary = false;

        return CONSTANT;
    }

    \n {
        throw CompilerException(CompilerExceptionSource::Syntax,
            "String is not terminated at the end of the line", yylloc.first_line, yylloc.first_column);
    }

    \\[0-7]{1,3} {
        // Octal escape sequence
        int32_t result;
        sscanf_s(yytext + 1, "%o", &result);

        if (result > 0xff) {
            throw CompilerException(CompilerExceptionSource::Syntax,
                "String escape sequence is out of bounds", yylloc.first_line, yylloc.first_column);
        }

        *string_buffer_ptr = result;
        string_buffer_ptr++;
    }

    \\[0-9]+ {
        throw CompilerException(CompilerExceptionSource::Syntax,
            "String escape sequence is not in octal format", yylloc.first_line, yylloc.first_column);
    }

    \\n  { *(string_buffer_ptr++) = '\n'; }
    \\t  { *(string_buffer_ptr++) = '\t'; }
    \\r  { *(string_buffer_ptr++) = '\r'; }
    \\b  { *(string_buffer_ptr++) = '\b'; }
    \\f  { *(string_buffer_ptr++) = '\f'; }

    \\(.|\n)  { *(string_buffer_ptr++) = yytext[1]; }

    [^\\\n\"]+ {
        // Everything but '\', '"' and new-line
        char* ptr = yytext;
        while (*ptr) {
            *(string_buffer_ptr++) = *(ptr++);
        }
    }
}

\' {
    string_buffer_ptr = string_buffer;
    BEGIN(STATE_CHAR);
}

<STATE_CHAR>{
    \' {
        BEGIN(INITIAL);

        if (string_buffer == string_buffer_ptr) {
            throw CompilerException(CompilerExceptionSource::Syntax,
                "Character literal must not be empty", yylloc.first_line, yylloc.first_column);
        }

        // Fill remaining places with zeroes
        *(string_buffer_ptr++) = '\0';
        *(string_buffer_ptr++) = '\0';

        LogDebug("L: Found character constant \"" << string_buffer << "\"");

        size_t length = strlen(string_buffer);
        BaseSymbolType type;
        uint32_t value;
        if (length > 4) {
            throw CompilerException(CompilerExceptionSource::Syntax,
                "Character literal is too long", yylloc.first_line, yylloc.first_column);
        } else if (length > 2) {
            type = BaseSymbolType::Uint32;
            value = *(uint32_t*)string_buffer;
        } else if (length > 1) {
            type = BaseSymbolType::Uint16;
            value = *(uint32_t*)string_buffer & 0xffff;
        } else {
            type = BaseSymbolType::Uint8;
            value = *(uint32_t*)string_buffer & 0xff;
        }

        yylval.expression.value = _strdup(std::to_string(value).c_str());
        yylval.expression.exp_type = ExpressionType::Constant;
        yylval.expression.type = { type, 0 };
        allow_unary = false;

        return CONSTANT;
    }

    \n {
        throw CompilerException(CompilerExceptionSource::Syntax,
            "Character literal is not terminated at the end of the line", yylloc.first_line, yylloc.first_column);
    }

    \\[0-7]{1,3} {
        // Octal escape sequence
        int32_t result;
        sscanf_s(yytext + 1, "%o", &result);

        if (result > 0xff) {
            throw CompilerException(CompilerExceptionSource::Syntax,
                "Character literal escape sequence is out of bounds", yylloc.first_line, yylloc.first_column);
        }

        *string_buffer_ptr = result;
        string_buffer_ptr++;
    }

    \\[0-9]+ {
        throw CompilerException(CompilerExceptionSource::Syntax,
            "Character literal escape sequence is not in octal format", yylloc.first_line, yylloc.first_column);
    }

    \\n  { *(string_buffer_ptr++) = '\n'; }
    \\t  { *(string_buffer_ptr++) = '\t'; }
    \\r  { *(string_buffer_ptr++) = '\r'; }
    \\b  { *(string_buffer_ptr++) = '\b'; }
    \\f  { *(string_buffer_ptr++) = '\f'; }

    \\(.|\n)  { *(string_buffer_ptr++) = yytext[1]; }

    [^\\\n\']+ {
        // Everything but '\', ''' and new-line
        char* ptr = yytext;
        while (*ptr) {
            *(string_buffer_ptr++) = *(ptr++);
        }
    }
}

%%
