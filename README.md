<h1 align="center">
    Cx Compiler
</h1>

<div align="center">
    Compiler for modified C language to i386 DOS executables
</div>

<div align="center">
  <sub>
    Brought to you by <a href="https://github.com/deathkiller">@deathkiller</a>
  </sub>
</div>
<hr/>


## Introduction
Compiler that supports modified version of C language and creates original DOS executables for i386 architecture. The executables run on MS-DOS, FreeDOS, 32-bit MS Windows using NTVDM, DosBox and other similar platforms. Currently supports several types of variables, pointers, dynamic memory allocation, branching and looping, includes and much more.

[![Build Status](https://img.shields.io/appveyor/ci/deathkiller/cx-compiler/master.svg?logo=data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCI+PHBhdGggZmlsbD0iI2ZmZmZmZiIgZD0iTTI0IDIuNXYxOUwxOCAyNCAwIDE4LjV2LS41NjFsMTggMS41NDVWMHpNMSAxMy4xMTFMNC4zODUgMTAgMSA2Ljg4OWwxLjQxOC0uODI3TDUuODUzIDguNjUgMTIgM2wzIDEuNDU2djExLjA4OEwxMiAxN2wtNi4xNDctNS42NS0zLjQzNCAyLjU4OXpNNy42NDQgMTBMMTIgMTMuMjgzVjYuNzE3eiI+PC9wYXRoPjwvc3ZnPg==)](https://ci.appveyor.com/project/deathkiller/cx-compiler)
[![Code Quality](https://img.shields.io/codacy/grade/06fe278f2bdf43768bcc3615a482e42a.svg?logo=codacy&logoColor=ffffff)](https://www.codacy.com/app/deathkiller/cx-compiler)
[![License](https://img.shields.io/github/license/deathkiller/cx-compiler.svg)](https://github.com/deathkiller/cx-compiler/blob/master/LICENSE)
[![Lines of Code](https://img.shields.io/badge/lines%20of%20code-10k-blue.svg)](https://github.com/deathkiller/cx-compiler/graphs/code-frequency)

Requires [Microsoft Visual Studio 2015](https://www.visualstudio.com/) or newer (or equivalent C++11 compiler) to build the solution.


## Usage
* Run `Compiler.exe "Path to source code" "Path to output executable" /target:dos` to compile specified source code file to executable.
* Run `Compiler.exe "Path to output executable" /target:dos` to use the compiler in interactive mode and write source code directly to command line/terminal.


## Example
```c
// Pointer types are allowed everywhere
bool BinarySearch(uint32* array, uint32 size, uint32 key, uint32* index) {
    uint32 low, high, mid;
    low = 0;
    high = (size - 1);
    while (low <= high) {
        mid = (low + high) / 2;
        if (key == array[mid]) {
            index[0] = mid;
            return true;
        }

        if (key < array[mid]) {
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    
    return false;
}

uint8 Main() {
    // Print to console
    PrintString("Example application - Binary search\r\n");
    PrintNewLine();
    PrintString("Enter size of array: ");
    // Read user input
    uint32 size = ReadUint32();
    PrintNewLine();

    // Allocate memory on heap
    uint32* array = alloc<uint32>(size);
    if (array == null) {
        PrintString("Cannot allocate required memory block!\r\n");
        return 1;
    }

    uint32 i;
    uint32 last = 0;
    for (i = 0; i < size; ++i) {
        PrintString("Enter #");
        PrintUint32(i + 1);
        PrintString(" number: ");
        array[i] = ReadUint32();
        PrintNewLine();
        
        if (array[i] < last) {
            PrintString("The number must be bigger than the previous one. Try it again!\r\n");
            --i;
        } else {
            last = array[i];
        }
    }

    PrintString("Enter number to find in the array: ");
    uint32 key = ReadUint32();
    PrintNewLine();
    
    uint32 index = 0;
    // Call method with one parameter passed by reference
    bool found = BinarySearch(array, size, key, &index);
    if (found == true) {
        PrintString("The number found on position: ");
        PrintUint32(index + 1);
        PrintNewLine();
    } else {
        PrintString("The number was not found!\r\n");
    }

    // Release allocated memory
    release(array);
    return 0;
}
```


## License
This project is licensed under the terms of the [GNU General Public License v3.0](./LICENSE).