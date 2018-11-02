#include "Compiler.h"

Compiler c;

int __cdecl wmain(int argc, wchar_t* argv[], wchar_t* envp[])
{
    return c.OnRun(argc, argv);
}