#include "GenericEmitter.h"

uint8_t* GenericEmitter::AllocateBuffer(uint32_t size)
{
    if (!buffer) {
        buffer_size = size + 256;
        buffer = (uint8_t*)malloc(buffer_size);
        buffer_offset = size;
        return buffer;
    }

    if (buffer_size < buffer_offset + size) {
        buffer_size = buffer_offset + size + 64;
        buffer = (uint8_t*)realloc(buffer, buffer_size);
    }

    uint32_t prev_offset = buffer_offset;
    buffer_offset += size;
    return buffer + prev_offset;
}

uint8_t* GenericEmitter::AllocateBufferForInstruction(uint32_t size)
{
    ip_dst += size;
    return AllocateBuffer(size);
}