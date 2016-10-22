#pragma once

#define LOG_ERROR(fmt,...)  printf("%u: Error: " fmt "\r\n", GetTickCount(),##__VA_ARGS__)
#define LOG_DEBUG(fmt,...)  printf("%u: [DEBUG] " fmt "\r\n", GetTickCount(),##__VA_ARGS__)
#define LOG(fmt,...)        printf("%u: " fmt "\r\n", GetTickCount(),##__VA_ARGS__)

#define LOG_MEMORY(fmt,...) //printf("%u: [MEMORY] " fmt "\r\n", GetTickCount(),##__VA_ARGS__)
#define LOG_NBD(fmt,...)    printf("%u: [NBD] " fmt "\r\n", GetTickCount(),##__VA_ARGS__)

#define LOG_NET_INFO(fmt,...)  printf("%u: [NET] " fmt "\r\n", GetTickCount(),##__VA_ARGS__)
#define LOG_NET_DEBUG(fmt,...)  printf("%u: [NET][DEBUG] " fmt "\r\n", GetTickCount(),##__VA_ARGS__)

class Wsa
{
  public:
    int error;
    Wsa()
    {
        WSADATA data;
        error = WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~Wsa()
    {
        if(error == 0) {
            WSACleanup();
        }
    }
};

struct String
{
    char* ptr;
    UINT length;
    String() : ptr(NULL), length(0)
    {
    }
    String(char* ptr, UINT length) : ptr(ptr), length(length)
    {
    }
    bool Equals(String other)
    {
        return this->length == other.length &&
          memcmp(this->ptr, other.ptr, other.length) == 0;
    }
};

// A "Zero-Terminated" string
struct NullTerminatedString : public String
{
    NullTerminatedString(char* ptr, UINT length) : String(ptr, length)
    {
    }
};
#define NullTerminatedStringFromLiteral(literal) NullTerminatedString(literal, sizeof(literal)-1)