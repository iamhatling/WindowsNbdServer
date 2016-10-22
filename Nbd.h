#pragma once

#define NBD_CMD_READ        0
#define NBD_CMD_WRITE       1
#define NBD_CMD_DISC        2
#define NBD_CMD_FLUSH       3
#define NBD_CMD_TRIM        4
#define NBD_CMD_WRITE_ZEROS 6

extern const char INITIAL_HANDSHAKE[];
extern const char *IHAVEOPT;
extern const char NBD_REQUEST_MAGIC[];
extern const char NBD_REPLY_MAGIC[];


#ifdef LITTLE_ENDIAN
    #define _1_NETWORK_ORDER                     0x01000000
    #define _2_NETWORK_ORDER                     0x02000000
    #define _3_NETWORK_ORDER                     0x03000000
    #define _4_NETWORK_ORDER                     0x04000000
    #define _5_NETWORK_ORDER                     0x05000000
    #define _6_NETWORK_ORDER                     0x06000000
    #define NBD_REQUEST_MAGIC_UINT_NETWORK_ORDER 0x13956025
#elif BIG_ENGIAN
    #define _1_NETWORK_ORDER                     0x00000001
    #define _2_NETWORK_ORDER                     0x00000002
    #define _3_NETWORK_ORDER                     0x00000003
    #define _4_NETWORK_ORDER                     0x00000004
    #define _5_NETWORK_ORDER                     0x00000005
    #define _6_NETWORK_ORDER                     0x00000006
    #define NBD_REQUEST_MAGIC_UINT_NETWORK_ORDER 0x25609513
#else
  #error Need to define LITTLE_ENDIAN or BIG_ENDIAN
#endif

#define NBD_CMD_READ_NETWORK_ORDER 0

#define SET_UINT(buffer, value) *((UINT*)(buffer)) = value
void AppendUint(char* buffer, UINT value);
void AppendUint64(char* buffer, UINT64 value);

struct WindowsBlockDevice
{
    NullTerminatedString name;
    UINT64 size;
    WindowsBlockDevice(NullTerminatedString name, UINT64 size)
        : name(name), size(size)
    {
    }
    virtual void Read(char* buffer, UINT length, UINT64 diskOffset) = 0;
    virtual void Write(char* buffer, UINT length, UINT64 diskOffset) = 0;
};
struct MemoryBlockDevice : public WindowsBlockDevice
{
    char* memoryPtr;
    MemoryBlockDevice(NullTerminatedString name, UINT64 size, char* memoryPtr)
        : WindowsBlockDevice(name, size), memoryPtr(memoryPtr)
    {
    }
    void Read(char* buffer, UINT length, UINT64 diskOffset);
    void Write(char* buffer, UINT length, UINT64 diskOffset);
};

