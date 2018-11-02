#pragma once

#include <exception>
#include <stdexcept>

// Common exceptions
#define ThrowOnUnreachableCode()    \
    __debugbreak();                 \
    throw CompilerException(CompilerExceptionSource::Compilation, "Unexpected compiler error");

enum struct CompilerExceptionSource {
    Unknown,

    Syntax,
    Declaration,
    Statement,

    Compilation
};

class CompilerException : public std::exception
{
public:
    CompilerException(CompilerExceptionSource source, std::string message)
        : source(source),
          message(message)
    {
    }

    CompilerException(CompilerExceptionSource source, std::string message, int32_t line, int32_t column)
        : source(source),
          message(message),
          line(line),
          column(column)
    {
    }

    virtual const char* what() const throw()
    {
        return message.c_str();
    }

    CompilerExceptionSource GetSource()
    {
        return source;
    }

    int32_t GetLine()
    {
        return line;
    }

    int32_t GetColumn()
    {
        return column;
    }

private:
    CompilerExceptionSource source;
    std::string message;
    int32_t line = -1;
    int32_t column = -1;

};