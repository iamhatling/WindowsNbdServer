#include <winsock2.h>
#include <stdio.h>
#include <stdarg.h>

#include "Common.h"
#include "Nbd.h"

char buffer[4096];

#define TEST_FAIL    0
#define TEST_SUCCESS 1

#define TEST_ASSERT(condition, line, fmt, ...)                              \
    do                                                                      \
    {                                                                       \
        if(!(condition))                                                    \
        {                                                                   \
            printf("TEST_ASSERT line %u: " fmt "\r\n", line,##__VA_ARGS__); \
            return TEST_FAIL;                                               \
        }                                                                   \
    } while(0);

struct Connection
{
    SOCKET so;
    Connection(unsigned short port)
    {
        sockaddr_in addr;
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(0x7F000001);

        so = socket(addr.sin_family, SOCK_STREAM, IPPROTO_TCP);
        if(so == INVALID_SOCKET)
        {
            printf("Error: socket function failed (e=%d)\r\n", GetLastError());
        }
        else
        {
            if(SOCKET_ERROR == connect(so, (sockaddr*)&addr, sizeof(addr)))
            {
                printf("Error: connect function failed (e=%d)\r\n", GetLastError());
                closesocket(so);
                so = INVALID_SOCKET;
            }
        }
    }
    ~Connection()
    {
        if(so != INVALID_SOCKET)
        {
            shutdown(so, SD_BOTH);
            closesocket(so);
            so = INVALID_SOCKET;
        }
    }
    SOCKET sock()
    {
        return so;
    }
};

// Returns: 0 on error
BOOL ReceiveFull(SOCKET so, char* buffer, UINT size)
{
    do
    {
        int received = recv(so, buffer, size, 0);
        if(received <= 0)
        {
            printf("Error: connection closed but expected %u bytes\r\n", size);
            return 0; // fail
        }
        size -= received;
        buffer += received;
    } while(size);
    return 1; // success
}

BOOL SendWithPattern(SOCKET so, char* buffer, UINT size, UINT* sendPattern, UINT sendPatternLength, DWORD sleepMillis)
{
    while(true)
    {
        for(UINT patternIndex = 0; patternIndex < sendPatternLength; patternIndex++)
        {
            UINT sendLength = sendPattern[patternIndex];
            if(sendLength > size)
            {
                goto FINISH;
            }
            printf("[DEBUG] sending %d bytes...\r\n", sendLength);
            int sent = send(so, buffer, sendLength, 0);
            TEST_ASSERT(sendLength == sent, __LINE__, "send %u returned %d (e=%d)", sendLength, sent, GetLastError());
            buffer += sendLength;
            size -= sendLength;
            if(sleepMillis)
            {
                Sleep(sleepMillis);
            }
        }
    }
  FINISH:
    if(size > 0)
    {
        printf("[DEBUG] sending %d bytes...\r\n", size);
        int sent = send(so, buffer, size, 0);
        TEST_ASSERT(size == sent, __LINE__, "send %u returned %u (e=%d)", size, sent, GetLastError());
    }
    return TEST_SUCCESS;
}
BOOL Send(SOCKET so, char* data, UINT size)
{
    printf("[DEBUG] sending %d bytes...\r\n", size);
    int sent = send(so, data, size, 0);
    TEST_ASSERT(size == sent, __LINE__, "send %u returned %u (e=%d)", size, sent, GetLastError());
    return TEST_SUCCESS;
}
BOOL Recv(SOCKET so, UINT size)
{
    while(size > sizeof(buffer))
    {
        int received = recv(so, buffer, sizeof(buffer), 0);
        TEST_ASSERT(sizeof(buffer) == received,
            __LINE__, "recv size=%u returned %d (e=%u)", sizeof(buffer), received, GetLastError());
        printf("[DEBUG] received %u bytes\r\n", received);
        size -= sizeof(buffer);
    }
    if(size > 0)
    {
        int received = recv(so, buffer, size, 0);
        TEST_ASSERT(size == received,
            __LINE__, "recv size=%u returned %d (e=%u)", size, received, GetLastError());
        printf("[DEBUG] received %u bytes\r\n", received);
    }
}

BOOL SendHandshake(Connection* conn, char* handshake, UINT size, UINT sleepMillis, UINT* sendPattern, UINT sendPatternLength)
{
    TEST_ASSERT(ReceiveFull(conn->so, buffer, 18), __LINE__, "ReceiveServerHandshake");
    TEST_ASSERT(memcmp(buffer, "NBDMAGICIHAVEOPT\0\0", 18) == 0, __LINE__, "Invalid server handshake");
    TEST_ASSERT(SendWithPattern(conn->so, handshake, size, sendPattern, sendPatternLength, sleepMillis), __LINE__, "SendWithPattern failed");
    return TEST_SUCCESS;
}
#define SendHandshakeLiteral(conn, lit,...) SendHandshake(conn, lit, sizeof(lit)-1,##__VA_ARGS__)

BOOL SendReadRequest(SOCKET so, UINT64 handle, UINT64 offset, UINT length)
{
    SET_UINT    (buffer +  0, NBD_REQUEST_MAGIC_UINT_NETWORK_ORDER);
    SET_UINT    (buffer +  4, NBD_CMD_READ_NETWORK_ORDER);
    AppendUint64(buffer +  8, handle);
    AppendUint64(buffer + 16, offset);
    AppendUint  (buffer + 24, length);
    TEST_ASSERT(Send(so, buffer, 28), __LINE__, "Send failed");
    return TEST_SUCCESS;
}



BOOL TestHandshakeAllSinglePatterns(char* handshake, UINT size, UINT sleepMillis, BOOL getResponse)
{
    for(UINT i = size; i > 0; i--)
    //for(UINT i = size; i <= size; i++)
    {
        Connection conn(10809);
        TEST_ASSERT(SendHandshake(&conn, handshake, size, sleepMillis, &i, 1), __LINE__, "SendHandshake failed");
        if(getResponse)
        {
            int received = recv(conn.so, buffer, sizeof(buffer), 0);
            TEST_ASSERT(134 == received, __LINE__, "expected recv to return %u, but returned %d (e=%d)", 134, received, GetLastError());
        }
        else
        {
            //int received = recv(conn.so, buffer, sizeof(buffer), 0);
            //TEST_ASSERT(0 == received, __LINE__, "expected recv to return %u, but returned %d (e=%d)", 0, received, GetLastError());
        }
    }
    return TEST_SUCCESS;
}
#define TestHandshakeAllSinglePatternsLiteral(lit,...) \
    TestHandshakeAllSinglePatterns(lit,sizeof(lit)-1,##__VA_ARGS__)

#define STANDARD_EXPORT_OPTION "IHAVEOPT\0\0\0\x01\0\0\0\6""export"

BOOL run()
{
    TEST_ASSERT(TestHandshakeAllSinglePatternsLiteral("\0\0\0\0IHAVEOPT\0\0\0\x02\0\0\0\0" STANDARD_EXPORT_OPTION, 10, TRUE),
        __LINE__, "TestHandshake failed");
    TEST_ASSERT(TestHandshakeAllSinglePatternsLiteral("\0\0\0\0IHAVEOPT\0\0\0\x02\0\0\0\0", 10, FALSE),
        __LINE__, "TestHandshake failed");
    TEST_ASSERT(TestHandshakeAllSinglePatternsLiteral("\0\0\0\0IHAVEOPT\0\0\0\x02\0\0\0\x01X" STANDARD_EXPORT_OPTION, 10, TRUE),
        __LINE__, "TestHandshake failed");
    TEST_ASSERT(TestHandshakeAllSinglePatternsLiteral("\0\0\0\0IHAVEOPT\0\0\0\x02\0\0\0\x02XY" STANDARD_EXPORT_OPTION, 10, TRUE),
        __LINE__, "TestHandshake failed");
    TEST_ASSERT(TestHandshakeAllSinglePatternsLiteral("\0\0\0\0IHAVEOPT\0\0\0\x02\0\0\0\x03XYZ" STANDARD_EXPORT_OPTION, 10, TRUE),
        __LINE__, "TestHandshake failed");

    {
        // Read a whole bunch of data
        Connection conn(10809);
        UINT64 handle = 0x138934934912;
        UINT sendSize = 10;
        TEST_ASSERT(SendHandshakeLiteral(&conn, "\0\0\0\0" STANDARD_EXPORT_OPTION, 0, &sendSize, 1),
            __LINE__, "SendHandshake failed");
        int received = recv(conn.so, buffer, sizeof(buffer), 0);
        TEST_ASSERT(134 == received, __LINE__, "expected recv to return %u, but returned %d (e=%d)", 134, received, GetLastError());

        // Test small read request
        TEST_ASSERT(SendReadRequest(conn.so, handle, 0, 1024), __LINE__, "SendReadRequest failed");
        TEST_ASSERT(Recv(conn.so, 1024 + 16), __LINE__, "Recv failed");

        // Test bigger read request
        TEST_ASSERT(SendReadRequest(conn.so, handle, 0, 65000), __LINE__, "SendReadRequest failed");
        TEST_ASSERT(Recv(conn.so, 65000 + 16), __LINE__, "Recv failed");

    }
    return TEST_SUCCESS;
}

int PerformanceTest(unsigned runCount, unsigned loopCount)
{
    volatile char buffer[4];

    LARGE_INTEGER frequency;
    if(!QueryPerformanceFrequency(&frequency))
    {
        LOG_ERROR("QueryPerformanceFrequency failed (e=%d)", GetLastError());
        return 1; // fail
    }
    LOG("frequency %llu", frequency.QuadPart);

    LARGE_INTEGER before;
    LARGE_INTEGER after;

    for(unsigned run = 0; run < runCount; run++)
    {
        if(!QueryPerformanceCounter(&before))
        {
            LOG_ERROR("QueryPerformanceCounter failed (e=%d)", GetLastError());
            return 1;
        }
        // Add test
        if(!QueryPerformanceCounter(&after))
        {
            LOG_ERROR("QueryPerformanceCounter failed (e=%d)", GetLastError());
            return 1;
        }
        //LOG("Test name: %llu", (after.QuadPart-before.QuadPart));
    }
    return TEST_SUCCESS;
}

int main(int argc, char* argv[])
{
    //PerformanceTest(3, 1000000000);

    Wsa wsa;
    if(wsa.error)
    {
        LOG_ERROR("WSAStartup failed (returned %d)", wsa.error);
        return 1;
    }

    int result = run();

    if(result == TEST_SUCCESS)
    {
        printf("SUCCESS\r\n");
        return 0;
    }
    else
    {
        printf("ERROR\r\n");
        return 1; // fail
    }
}