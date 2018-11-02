<h2 align="center">
    Cx Compiler
</h2>

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

[![License](https://img.shields.io/github/license/deathkiller/cx-compiler.svg)](https://github.com/deathkiller/cx-compiler/blob/master/LICENSE)
[![Lines of Code](https://tokei.rs/b1/github/deathkiller/cx-compiler)](https://github.com/deathkiller/cx-compiler)

Requires [Microsoft Visual Studio 2015](https://www.visualstudio.com/) or newer (or equivalent C++11 compiler) to build the solution.


## Usage
* Run `Compiler.exe "Path to source code" "Path to output executable" /target:dos` to compile specified source code file to executable.
* Run `Compiler.exe "Path to output executable" /target:dos` to use the compiler in interactive mode and write source code directly to command line/terminal.


## Example
```c
uint8 Main() {
    PrintString("Example application - Binary search\r\n");
    PrintNewLine();
    PrintString("Enter size of array: ");
    uint32 size = ReadUint32();
    PrintNewLine();

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

    uint32 low, high, mid;
    low = 0;
    high = (size - 1);
    bool found = false;
    while (low <= high) {
        mid = (low + high) / 2;
        if (key == array[mid]) {
            found = true;
            break;
        }

        if (key < array[mid]) {
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }

    release(array);
    
    PrintNewLine();
    if (found == true) {
        PrintString("The number found on position: ");
        PrintUint32(mid + 1);
        PrintNewLine();
        return 0;
    } else {
        PrintString("The number was not found!\r\n");
        return 1;
    }
}
```


## License
This project is licensed under the terms of the [GNU General Public License v3.0](./LICENSE).