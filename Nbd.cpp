#include <windows.h>
#include <stdio.h>

#include "Common.h"
#include "Nbd.h"

const char INITIAL_HANDSHAKE[] = "NBDMAGICIHAVEOPT";
const char *IHAVEOPT = INITIAL_HANDSHAKE + 8;
const char NBD_REQUEST_MAGIC[] = "\x25\x60\x95\x13";
const char NBD_REPLY_MAGIC[]   = "\x67\x44\x66\x98";

void AppendUint(char* buffer, UINT value)
{
    buffer[0] = (char)(value >> 24);
    buffer[1] = (char)(value >> 16);
    buffer[2] = (char)(value >> 8);
    buffer[3] = (char)value;
}
void AppendUint64(char* buffer, UINT64 value)
{
    buffer[0] = (char)(value >> 56);
    buffer[1] = (char)(value >> 48);
    buffer[2] = (char)(value >> 40);
    buffer[3] = (char)(value >> 32);
    buffer[4] = (char)(value >> 24);
    buffer[5] = (char)(value >> 16);
    buffer[6] = (char)(value >> 8);
    buffer[7] = (char)value;
}


void MemoryBlockDevice::Read(char* buffer, UINT length, UINT64 diskOffset)
{
    LOG_DEBUG("MemoryBlockDevice read(offset=%llu, length=%u)", diskOffset, length);
    memcpy(buffer, memoryPtr + diskOffset, length);
}
void MemoryBlockDevice::Write(char* buffer, UINT length, UINT64 diskOffset)
{
    LOG_DEBUG("MemoryBlockDevice write(offset=%llu, length=%u)", diskOffset, length);
    memcpy(memoryPtr + diskOffset, buffer, length);
}
