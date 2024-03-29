#pragma once

#define DO_STRINGIFY(x) #x
#define STRINGIFY(x) DO_STRINGIFY(x)

#define VERSION_MAJOR   0
#define VERSION_MINOR   6
#define VERSION_BUILD   0
#define VERSION_REV     0


#define VERSION_NAME                 "Cx Compiler"
#define VERSION_FILEVERSION_NUM      VERSION_MAJOR,VERSION_MINOR,VERSION_BUILD,VERSION_REV
#define VERSION_FILEVERSION          STRINGIFY(VERSION_MAJOR) "." STRINGIFY(VERSION_MINOR) "." \
                                     STRINGIFY(VERSION_BUILD) "." STRINGIFY(VERSION_REV)
