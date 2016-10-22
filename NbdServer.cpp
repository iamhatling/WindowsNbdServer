#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>

#include "Common.h"
#include "SelectServer.h"
#include "Nbd.h"

// TODO: log settings
// --------------------------------------------------------
//
// Categories
// --------------------------------------------------------
// socket (select server log)
// network (tcp connections, and tcp/udp sends/receives)
//
// Levels
// --------------------------------------------------------
// error
// debug
// info
//


static char sharedResponseBuffer[16];
#define SHARED_BUFFER_SIZE 8192
static char globalSharedBuffer[SHARED_BUFFER_SIZE];

#define MAX_IP_STRING   39 // A full IPv6 string like 2001:0db8:85a3:0000:0000:8a2e:0370:7334
#define MAX_PORT_STRING  5 // 65535
#define MAX_ADDR_STRING (MAX_IP_STRING + 1 + MAX_PORT_STRING)

#define LITERAL_LENGTH(str) (sizeof(str)-1)
#define STATIC_ARRAY_LENGTH(arr) (sizeof(arr)/sizeof(arr[0]))

struct Export
{
    NullTerminatedString name;
    UINT64 size;
    Export(NullTerminatedString name, UINT64 size)
        : name(name), size(size)
    {
    }
};

char exportMemoryDeviceBuffer[1024*100]; // 100 KB
MemoryBlockDevice exportMemoryDevice(NullTerminatedStringFromLiteral("export"),
    sizeof(exportMemoryDeviceBuffer), exportMemoryDeviceBuffer);
//FileBlockDevice(NullTerminatedStringFromLiteral("export"), 100*1024*1024, "C:\\temp\\blockdevice"), // 100 MB
WindowsBlockDevice* exportedDevices[] = {
    &exportMemoryDevice,
};


void AddrToString(char dest[], sockaddr* addr)
{
    if(addr->sa_family == AF_INET)
    {
        unsigned short port = htons(((sockaddr_in*)addr)->sin_port);
        u_long ipv4 = htonl(((sockaddr_in*)addr)->sin_addr.s_addr);
        sprintf(dest, "%u.%u.%u.%u:%u",
            ipv4 >> 24, (ipv4 >> 16) & 0xFF, (ipv4 >> 8) & 0xFF,
            ipv4 & 0xFF, port);
    }
    else if(addr->sa_family == AF_INET6)
    {
        unsigned short port = htons(((sockaddr_in6*)addr)->sin6_port);
        sprintf(dest, "<ipv6-addr>:%u", port);
    }
    else
    {
        sprintf(dest, "<address-family:%d>", addr->sa_family);
    }
}

int sendWithLog(const char* context, SOCKET so, char* buffer, UINT length)
{
    int sent = send(so, buffer, length, 0);
    if(sent != length)
    {
        if(sent < 0)
        {
            LOG_ERROR("%s(s=%u) send %u bytes failed (returned %d, e=%d)", context, so, length, sent, GetLastError());
        }
        else
        {
            LOG_ERROR("%s(s=%u) send %u bytes returned %d (e=%d)", context, so, length, sent, GetLastError());
        }
    }
    return sent;
}

struct HeapBuffer
{
    //#define DEFAULT_INITIAL_LENGTH 1 // Use this value to verify it works correctly
    #define DEFAULT_INITIAL_LENGTH 256
    char* ptr;
    UINT length;
    HeapBuffer() : ptr(NULL), length(0)
    {
    }
    ~HeapBuffer()
    {
        if(ptr)
        {
            LOG_MEMORY("~HeapBuffer: HeapFree (ptr=%p, length=%u)", ptr, length);
            HeapFree(GetProcessHeap(), HEAP_NO_SERIALIZE, ptr);
            ptr = NULL;
            length = 0;
        }
        else
        {
            //LOG_MEMORY("~HeapBuffer: nothing to free");
        }
    }
    void EnsureCapacity(UINT capacity, UINT preserveLength)
    {
        if(capacity > length)
        {
            if(length == 0)
            {
                length = (capacity < DEFAULT_INITIAL_LENGTH) ? DEFAULT_INITIAL_LENGTH : capacity;
                ptr = (char*)HeapAlloc(GetProcessHeap(), HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS, length);
                LOG_MEMORY("EnsureCapacity(%u) HeapAlloc(size=%u) returned %p", capacity, length, ptr);
            }
            else
            {
                UINT newLength = 2*length;
                if(capacity > newLength)
                {
                    newLength = capacity;
                }
                if(HeapReAlloc(GetProcessHeap(), HEAP_NO_SERIALIZE | HEAP_REALLOC_IN_PLACE_ONLY, ptr, newLength))
                {
                    LOG_MEMORY("EnsureCapacity(%u) HeapReAlloc grew buffer %p from %u to %u in place",
                        capacity, ptr, length, newLength);
                }
                else
                {
                    LPVOID newBuffer = HeapAlloc(GetProcessHeap(), HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS, newLength);
                    LOG_MEMORY("EnsureCapacity(%u) HeapReAlloc failed in place, HeapAlloc(size=%u) returned %p)",
                        capacity, length, ptr);
                    if(preserveLength)
                    {
                        memcpy(newBuffer, ptr, preserveLength);
                    }
                    HeapFree(GetProcessHeap(), HEAP_NO_SERIALIZE, ptr);
                    ptr = (char*)newBuffer;
                }
                length = newLength;
            }
        }
    }
};



// Returns: NULL on error
WindowsBlockDevice* ToTransmissionMode(SelectSock* sock, char* sharedBuffer, String exportName)
{
    unsigned exportIndex;
    for(exportIndex = 0; exportIndex < STATIC_ARRAY_LENGTH(exportedDevices); exportIndex++)
    {
        if(exportName.Equals(exportedDevices[exportIndex]->name))
        {
            break;
        }
    }

    if(exportIndex == STATIC_ARRAY_LENGTH(exportedDevices))
    {
        LOG_NBD("ToTransmissionMode: client requested export '%.*s' but it was not found", exportName.length, exportName.ptr);
        return NULL; // error (no block device)
    }
    LOG_NBD("ToTransmissionMode: export name \"%.*s\" was found!", exportName.length, exportName.ptr);

    // NOTE: do not use exportName, because modifying
    // the shared buffer will modify the contents of exportName!
    exportName.ptr = NULL; // set this just in case!

    // Setup reply
    AppendUint64(sharedBuffer, exportedDevices[exportIndex]->size);
    sharedBuffer[8] = 0; // transmission flags
    sharedBuffer[9] = 0;
    ZeroMemory(sharedBuffer + 10, 124);
    // total size is 134
    int sent = send(sock->so, sharedBuffer, 134, 0);
    if(sent != 134)
    {
        LOG_ERROR("send(size=134) returned %d (e=%u)", sent, GetLastError());
        return NULL; // error (send failed)
    }

    return exportedDevices[exportIndex];
}

#define NBD_OPT_EXPORT_NAME      1
#define NBD_OPT_ABORT            2
#define NBD_OPT_LIST             3
#define NBD_OPT_PEEK_EXPORT      4
#define NBD_OPT_STARTTLS         5
#define NBD_OPT_INFO             6
#define NBD_OPT_GO               7
#define NBD_OPT_STRUCTURED_REPLY 8
#define NBD_OPT_BLOCK_SIZE       9


struct RequestParams
{
    // ordered bigger types first
    UINT64 handle;
    UINT64 offset;
    UINT length;
};

struct ReadRequest
{
    RequestParams params;
    UINT headersSent;
    ReadRequest(RequestParams* params) : params(*params), headersSent(0)
    {
    }
};

struct ReadRequestQueue
{
    HeapBuffer* heapBuffer;
    UINT count;
    ReadRequestQueue(HeapBuffer* heapBuffer) : heapBuffer(heapBuffer), count(0)
    {
    }
    void Add(RequestParams* requestParams)
    {
        UINT currentSize = count*sizeof(ReadRequest);
        heapBuffer->EnsureCapacity(currentSize+sizeof(ReadRequest), currentSize);
        ((ReadRequest*)heapBuffer->ptr)[count] = ReadRequest(requestParams);
        LOG_NBD("Added read request (queueIndex=%u offset=%llu length=%u handle=%016llx)",
            count, requestParams->offset, requestParams->length, requestParams->handle);
        count++;
    }
    // Returns: non-zero if connection should be closed
    BOOL HandleRequests(WindowsBlockDevice* device, SelectSock* sock, char* buffer)
    {
        // Process all read requests
        for(UINT i = 0; i < count; i++)
        {
            ReadRequest* request = ((ReadRequest*)heapBuffer->ptr) + i;

            if(request->headersSent < 16)
            {
                memcpy      (buffer +  0, NBD_REPLY_MAGIC, 4);
                SET_UINT    (buffer +  4, 0); // no error
                AppendUint64(buffer +  8, request->params.handle);

                UINT dataReadSize = request->params.length;
                if(dataReadSize > SHARED_BUFFER_SIZE - 16)
                {
                    dataReadSize = SHARED_BUFFER_SIZE - 16;
                }
                device->Read(buffer + 16, dataReadSize, request->params.offset);

                UINT sendLength = 16 + dataReadSize - request->headersSent;
                int sent = send(sock->so, buffer + request->headersSent, sendLength, 0);
                if(sent != sendLength)
                {
                    // TODO: add socket to select write set, and come back later
                    LOG("TransmissionHandler: send(size=%u) returned %d (e=%u)",
                        sendLength, sent, GetLastError());
                    return 1; // disconnect
                }
                LOG_NET_DEBUG("Sent %u bytes for read request", sendLength);
                request->headersSent = 16;
                request->params.offset += dataReadSize;
                request->params.length -= dataReadSize;
            }

            while(1)
            {
                UINT length = request->params.length;
                if(length == 0)
                {
                    LOG_NBD("Finished sending read reply for 0x%016llx", request->params.handle);
                    break;
                }
                if(length > SHARED_BUFFER_SIZE)
                {
                    length = SHARED_BUFFER_SIZE;
                }
                device->Read(buffer, length, request->params.offset);

                int sent = send(sock->so, buffer, length, 0);
                if(sent != length)
                {
                    // TODO: add socket to select write set, and come back later
                    LOG("TransmissionHandler: send(size=%u) returned %d (e=%u)",
                        length, sent, GetLastError());
                    return 1; // disconnect
                }
                LOG_NET_DEBUG("Sent %u bytes for read request", length);
                request->params.offset += length;
                request->params.length -= length;
            }
        }

        // TODO: this may not always be the case
        count = 0;
        return 0;
    }
};


struct Connection
{
    BOOL (Connection::*currentHandler)(SelectSock* sock, char* buffer, UINT size);
    UINT state;

    union
    {
        UINT clientFlags;
        struct
        {
            UINT optionID;
            UINT optionLength;
            // TODO: might need a field to save some flags like NBD_FLAG_C_NO_ZEROS
            //       or whether the "fixed newstyle negotiation" is being used
        } handshake;
        struct
        {
            RequestParams params;
            UINT commandFlagsAndType;
        } request;
    } dataUnion;

    HeapBuffer heapBuffer;
    WindowsBlockDevice* blockDevice;
    ReadRequestQueue readRequestQueue;

    Connection() : currentHandler(&Connection::HandshakeHandler),
        state(0), heapBuffer(), readRequestQueue(&heapBuffer)
    {
        dataUnion.clientFlags = 0; // This is the only part of the union
                                   // that needs to be initialized
    }
    ~Connection()
    {
        LOG_DEBUG("~Connection()");
    }

  private:
    // Returns: non-zero if connection should be closed
    BOOL HandshakeHandler(SelectSock* sock, char* buffer, UINT size)
    {
        char* saveBufferPtr = buffer;
        char* optionStringPtr = NULL;
        while(true)
        {
            switch(state)
            {
                // Get Client Flags (4 bytes)
                case 0:
                case 1:
                case 2:
                case 3:
                {
                    UINT consumeSize = 4 - state;
                    if(consumeSize > size)
                    {
                        consumeSize = size;
                    }
                    for(UINT i = 0; i < consumeSize; i++)
                    {
                        dataUnion.clientFlags <<= 8;
                        dataUnion.clientFlags |= (unsigned char)buffer[i];
                    }
                    state += consumeSize;
                    if(state < 4)
                    {
                        LOG_DEBUG("parsed all current data (state=%u)", state);
                        return 0; // parsed all current data
                    }
                    LOG_DEBUG("state=%u, clientflags = 0x%08x", state, dataUnion.clientFlags);
                    if(dataUnion.clientFlags)
                    {
                        LOG("Unknown client flags 0x%08x", dataUnion.clientFlags);
                        return 1; // close connection
                    }
                    size -= consumeSize;
                    buffer += consumeSize;
                }
                // Get "IHAVEOPT" magic (8 bytes)
                case 4:
                case 5:
                case 6:
                case 7:
                case 8:
                case 9:
                case 10:
                case 11:
                {
                    UINT consumeSize = 12 - state;
                    if(consumeSize > size)
                    {
                        consumeSize = size;
                    }
                    for(UINT i = 0; i < consumeSize; i++)
                    {
                        if(buffer[i] != IHAVEOPT[state-4+i])
                        {
                            LOG_ERROR("Invalid client option magic");
                            return 1; // close connection
                        }
                    }
                    state += consumeSize;
                    if(state < 12)
                    {
                        LOG_DEBUG("parsed all current data (state=%u)", state);
                        return 0; // parsed all current data
                    }
                    LOG_DEBUG("state=%u", state);
                    size -= consumeSize;
                    buffer += consumeSize;

                    // initialize next variable that will be received
                    dataUnion.handshake.optionID = 0;
                }
                // Get option id (4 bytes)
                case 12:
                case 13:
                case 14:
                case 15:
                {
                    UINT consumeSize = 16 - state;
                    if(consumeSize > size)
                    {
                        consumeSize = size;
                    }
                    for(UINT i = 0; i < consumeSize; i++)
                    {
                        dataUnion.handshake.optionID <<= 8;
                        dataUnion.handshake.optionID |= (unsigned char)buffer[i];
                    }
                    state += consumeSize;
                    if(state < 16)
                    {
                        LOG_DEBUG("parsed all current data (state=%u)", state);
                        return 0; // parsed all current data
                    }
                    LOG_DEBUG("state=%u, optionID is %u", state, dataUnion.handshake.optionID);
                    size -= consumeSize;
                    buffer += consumeSize;

                    // initialize next variable that will be received
                    dataUnion.handshake.optionLength = 0;
                }
                // Get option length (4 bytes)
                case 16:
                case 17:
                case 18:
                case 19:
                {
                    UINT consumeSize = 20 - state;
                    if(consumeSize > size)
                    {
                        consumeSize = size;
                    }
                    for(UINT i = 0; i < consumeSize; i++)
                    {
                        dataUnion.handshake.optionLength <<= 8;
                        dataUnion.handshake.optionLength |= (unsigned char)buffer[i];
                    }
                    state += consumeSize;
                    if(state < 20)
                    {
                        LOG_DEBUG("parsed all current data (state=%u)", state);
                        return 0; // parsed all current data
                    }
                    LOG_DEBUG("state=%u, optionLength is %u", state, dataUnion.handshake.optionLength);
                    // TODO: should probably have an option for a max option length
                    //       to prevent DDOS attacks from causing the server
                    //       to allocate a bunch of memory
                    size -= consumeSize;
                    buffer += consumeSize;

                    // Special case that prevents the need to allocate a heap buffer
                    // if the entire open data is already received
                    if(size >= dataUnion.handshake.optionLength)
                    {
                        optionStringPtr = buffer;
                        size -= dataUnion.handshake.optionLength;
                        buffer += dataUnion.handshake.optionLength;
                    }
                    else
                    {
                        optionStringPtr = NULL;
                        heapBuffer.EnsureCapacity(dataUnion.handshake.optionLength, 0);
                    }
                }
                // Get option data (<option-length> bytes)
                default:
                {
                    if(optionStringPtr == NULL)
                    {
                        UINT consumeSize = dataUnion.handshake.optionLength + 20 - state;
                        if(consumeSize > size)
                        {
                            consumeSize = size;
                        }
                        optionStringPtr = heapBuffer.ptr;
                        for(UINT i = 0; i < consumeSize; i++)
                        {
                            optionStringPtr[state-20+i] = buffer[i];
                        }
                        state += consumeSize;
                        if(state < 20 + dataUnion.handshake.optionLength)
                        {
                            LOG_DEBUG("parsed all current data (state=%u)", state);
                            return 0; // parsed all current data
                        }
                        size -= consumeSize;
                        buffer += consumeSize;
                    }

                    if(dataUnion.handshake.optionID == NBD_OPT_EXPORT_NAME)
                    {
                        if(size > 0)
                        {
                            LOG("Client sent too much data (still have %u bytes)", size);
                            return 1; // close connection (TODO: add test for this)
                        }
                        blockDevice = ToTransmissionMode(sock, saveBufferPtr,
                            String(optionStringPtr, dataUnion.handshake.optionLength));
                        if(!blockDevice)
                        {
                            return 1; // error
                        }
                        state = 0;
                        currentHandler = &Connection::TransmissionHandler;
                        return 0; // success (enter transmission mode)
                    }
                    LOG_DEBUG("option(%u bytes)=\"%.*s\"", dataUnion.handshake.optionLength,
                        dataUnion.handshake.optionLength, optionStringPtr);
                    state = 4; // set state to get the next option
                    break;
                }
            }
        }
    }
    // Returns: non-zero if connection should be closed
    BOOL TransmissionHandler(SelectSock* sock, char* buffer, UINT size)
    {
        char* saveBufferPtr = buffer;
        while(true)
        {
            switch(state)
            {
                // Get Magic
                case 0:
                case 1:
                case 2:
                case 3:
                {
                    UINT consumeSize = 4 - state;
                    if(consumeSize > size)
                    {
                        consumeSize = size;
                    }
                    for(UINT i = 0; i < consumeSize; i++)
                    {
                        if(buffer[i] != NBD_REQUEST_MAGIC[state-0+i])
                        {
                            LOG_ERROR("Invalid client request magic (expected 0x%02x, got 0x%02x)",
                                NBD_REQUEST_MAGIC[state-0+i], buffer[i]);
                            return 1; // close connection
                        }
                    }
                    state += consumeSize;
                    if(state < 4)
                    {
                        LOG_DEBUG("parsed all current data (state=%u)", state);
                        return 0; // parsed all current data
                    }
                    LOG_DEBUG("state=%u", state);
                    size -= consumeSize;
                    buffer += consumeSize;

                    // initialize next variable that will be received
                    dataUnion.request.commandFlagsAndType = 0;
                }
                // Get command flags and type
                case 4:
                case 5:
                case 6:
                case 7:
                {
                    UINT consumeSize = 8 - state;
                    if(consumeSize > size)
                    {
                        consumeSize = size;
                    }
                    for(UINT i = 0; i < consumeSize; i++)
                    {
                        dataUnion.request.commandFlagsAndType <<= 8;
                        dataUnion.request.commandFlagsAndType |= (unsigned char)buffer[i];
                    }
                    state += consumeSize;
                    if(state < 8)
                    {
                        LOG_DEBUG("parsed all current data (state=%u)", state);
                        return 0; // parsed all current data
                    }
                    LOG_DEBUG("state=%u flags=0x%04x command=%u",
                        state, dataUnion.request.commandFlagsAndType >> 16,
                        dataUnion.request.commandFlagsAndType & 0xFFFF);
                    size -= consumeSize;
                    buffer += consumeSize;

                    // initialize next variable that will be received
                    dataUnion.request.params.handle = 0;
                }
                // Get handle
                case 8:
                case 9:
                case 10:
                case 11:
                case 12:
                case 13:
                case 14:
                case 15:
                {
                    UINT consumeSize = 16 - state;
                    if(consumeSize > size)
                    {
                        consumeSize = size;
                    }
                    for(UINT i = 0; i < consumeSize; i++)
                    {
                        dataUnion.request.params.handle <<= 8;
                        dataUnion.request.params.handle |= (unsigned char)buffer[i];
                    }
                    state += consumeSize;
                    if(state < 16)
                    {
                        LOG_DEBUG("parsed all current data (state=%u)", state);
                        return 0; // parsed all current data
                    }
                    LOG_DEBUG("state=%u handle=0x%016llx", state, dataUnion.request.params.handle);
                    size -= consumeSize;
                    buffer += consumeSize;

                    // initialize next variable that will be received
                    dataUnion.request.params.offset = 0;
                }
                // Get offset
                case 16:
                case 17:
                case 18:
                case 19:
                case 20:
                case 21:
                case 22:
                case 23:
                {
                    UINT consumeSize = 24 - state;
                    if(consumeSize > size)
                    {
                        consumeSize = size;
                    }
                    for(UINT i = 0; i < consumeSize; i++)
                    {
                        dataUnion.request.params.offset <<= 8;
                        dataUnion.request.params.offset |= (unsigned char)buffer[i];
                    }
                    state += consumeSize;
                    if(state < 24)
                    {
                        LOG_DEBUG("parsed all current data (state=%u)", state);
                        return 0; // parsed all current data
                    }
                    LOG_DEBUG("state=%u offset=%llu", state, dataUnion.request.params.offset);
                    size -= consumeSize;
                    buffer += consumeSize;

                    // initialize next variable that will be received
                    dataUnion.request.params.length = 0;
                }
                // Get length
                case 24:
                case 25:
                case 26:
                case 27:
                {
                    UINT consumeSize = 28 - state;
                    if(consumeSize > size)
                    {
                        consumeSize = size;
                    }
                    for(UINT i = 0; i < consumeSize; i++)
                    {
                        dataUnion.request.params.length <<= 8;
                        dataUnion.request.params.length |= (unsigned char)buffer[i];
                    }
                    state += consumeSize;
                    if(state < 28)
                    {
                        LOG_DEBUG("parsed all current data (state=%u)", state);
                        return 0; // parsed all current data
                    }
                    LOG_DEBUG("state=%u length=%u", state, dataUnion.request.params.length);
                    size -= consumeSize;
                    buffer += consumeSize;

                    switch(dataUnion.request.commandFlagsAndType & 0xFFFF)
                    {
                        case NBD_CMD_READ:  // 0
                            readRequestQueue.Add(&dataUnion.request.params);
                            break;
                        case NBD_CMD_WRITE: // 1
                            LOG_NBD("Write request (offset=%llu length=%u handle=%016llx)",
                                dataUnion.request.params.offset,
                                dataUnion.request.params.length,
                                dataUnion.request.params.handle);
                            state = 28;
                            goto RECEIVE_WRITE_STATE;
                        case NBD_CMD_DISC:  // 2
                            LOG("TransmissionHandler: NBD_CMD_DISC not implemented");
                            return 1;
                        case NBD_CMD_FLUSH: // 3
                            LOG("TransmissionHandler: NBD_CMD_FLUSH not implemented");
                            return 1;
                        case NBD_CMD_TRIM:  // 4
                            LOG("TransmissionHandler: NBD_CMD_TRIM not implemented");
                            return 1;
                        case 5:
                          goto DEFAULT_CASE;
                        case NBD_CMD_WRITE_ZEROS: // 6
                            LOG("TransmissionHandler: NBD_CMD_WRITE_ZEROS not implemented");
                            return 1;
                        default:
                          DEFAULT_CASE:
                            LOG("TransmissionHandler: Unknown command %u",
                                dataUnion.request.commandFlagsAndType & 0xFFFF);
                            return 1;
                    }

                    state = 0;
                    break; // get next command
                }
                case 28: //  Receiving write data
                RECEIVE_WRITE_STATE:
                {
                    UINT consumeSize = dataUnion.request.params.length;
                    if(consumeSize > size)
                    {
                        consumeSize = size;
                    }
                    blockDevice->Write(buffer, consumeSize, dataUnion.request.params.offset);
                    size -= consumeSize;
                    buffer += consumeSize;

                    dataUnion.request.params.length -= consumeSize;
                    dataUnion.request.params.offset += consumeSize;
                    if(dataUnion.request.params.length > 0)
                    {
                        LOG_DEBUG("need %u more byte for write request", dataUnion.request.params.length);
                        return 0; // parsed all current data
                    }

                    // Send write response
                    {
                        memcpy      (sharedResponseBuffer +  0, NBD_REPLY_MAGIC, 4);
                        SET_UINT    (sharedResponseBuffer +  4, 0); // no error
                        AppendUint64(sharedResponseBuffer +  8, dataUnion.request.params.handle);
                        int sent = send(sock->so, sharedResponseBuffer, 16, 0);
                        if(sent != 16)
                        {
                            // TODO: add socket to select write set, and come back later
                            LOG("TransmissionHandler: send(size=%u) returned %d (e=%u)",
                                16, sent, GetLastError());
                            return 1; // disconnect
                        }
                        LOG_NET_DEBUG("Sent %u bytes for write request", 16);
                    }

                    state = 0;
                    break; // get next command
                }
                default:
                    LOG("TransmissionHandler(CodeBug) unknown state %u", state);
                    return 1;
            }
        }
    }
};


void TcpRecvHandler(SynchronizedSelectServer server, SelectSock* sock, PopReason reason, char* sharedBuffer)
{
    Connection* conn = (Connection*)sock->user;
    int size = recv(sock->so, (char*)sharedBuffer, SHARED_BUFFER_SIZE, 0);
    if(size <= 0)
    {
        if(size == 0)
        {
            LOG_NET_INFO("TcpRecvHandler(s=%d) client closed", sock->so);
        }
        else
        {
            LOG_NET_INFO("TcpRecvHandler(s=%d) recv returned error (return=%d, e=%d)", sock->so, size, GetLastError());
        }
        goto ERROR_EXIT;
    }

    LOG_NET_DEBUG("Got %u bytes", size);
    if((conn->*(conn->currentHandler))(sock, sharedBuffer, size))
    {
        //LOG_DEBUG("connection handler returned close");
        goto ERROR_EXIT; // error already logged
    }

    if(conn->readRequestQueue.HandleRequests(conn->blockDevice, sock, sharedBuffer))
    {
        //LOG_DEBUG("readRequestQueue handler returned close");
        goto ERROR_EXIT; // error already logged
    }

    return;

  ERROR_EXIT:
    LOG_NET_DEBUG("TcpRecvHandler(s=%u) closing", sock->so);
    if(conn)
    {
        //LOG_DEBUG("TcpRecvHandler(s=%u) deleting sock->user %p...", sock->so, sock->user);
        delete conn;
        sock->user = NULL;
    }
    shutdown(sock->so, SD_BOTH);
    closesocket(sock->so);
    sock->UpdateEventFlags(SelectSock::NONE);
}

void TcpAcceptHandler(SynchronizedSelectServer server, SelectSock* sock, PopReason reason, char* sharedBuffer)
{
    sockaddr_in addr;
    int addrSize = sizeof(addr);
    SOCKET newSock = accept(sock->so, (sockaddr*)&addr, &addrSize);
    if(INVALID_SOCKET == newSock)
    {
        LOG_NET_INFO("TcpAcceptHandler(s=%d) accept failed (e=%d)", sock->so, GetLastError());
    }
    char addrString[MAX_ADDR_STRING+1];
    AddrToString(addrString, (sockaddr*)&addr);

    LOG_NET_INFO("TcpAcceptHandler(s=%d) accepted new connection (s=%d) from '%s'", sock->so, newSock, addrString);
    memcpy(sharedBuffer, INITIAL_HANDSHAKE, 16);
    sharedBuffer[16] = 0;
    sharedBuffer[17] = 0;

    int sent = sendWithLog("TcpAcceptHandler", newSock, sharedBuffer, 18);
    if(sent != 18)
    {
        goto ERROR_EXIT; // error already logged
    }

    Connection* connection = new Connection();
    //LOG_DEBUG("Created connection at %p", connection);
    if(server.TryAddSock(SelectSock(newSock, connection, &TcpRecvHandler, SelectSock::READ, SelectSock::INF)))
    {
        LOG_NET_INFO("TcpAcceptHandler(s=%d) server full, rejected socket (s=%d) from '%s'", sock->so, newSock, addrString);
        goto ERROR_EXIT;
    }
    return;

  ERROR_EXIT:
    LOG_NET_DEBUG("TcpAcceptHandler(s=%u) closing", sock->so);
    shutdown(sock->so, SD_BOTH);
    closesocket(sock->so);
    sock->UpdateEventFlags(SelectSock::NONE);
}

#define NBD_PORT     10809
#define LISTEN_BACKLOG 8

int RunNbdServer()
{
    SelectServer server;
    {
        // TODO: I don't like that I have to lock the server
        //       because it hasn't started yet.
        LockedSelectServer locked(&server);
        {
            sockaddr_in addr;
            addr.sin_family = AF_INET;

            {
                SOCKET so = socket(addr.sin_family, SOCK_STREAM, IPPROTO_TCP);
                if(so == INVALID_SOCKET)
                {
                    LOG_ERROR("socket function failed (e=%d)", GetLastError());
                    return 1; // error
                }
                addr.sin_port = htons(NBD_PORT);
                addr.sin_addr.s_addr = 0;
                LOG("(s=%d) Adding TCP Listener on port %u", so, NBD_PORT);
                if(SOCKET_ERROR == bind(so, (sockaddr*)&addr, sizeof(addr)))
                {
                    LOG_ERROR("bind failed (e=%d)", GetLastError());
                    return 1; // error
                }
                if(SOCKET_ERROR == listen(so, LISTEN_BACKLOG))
                {
                    LOG_ERROR("listen failed (e=%d)", GetLastError());
                    return 1; // error
                }
                locked.TryAddSock(SelectSock(so, NULL, &TcpAcceptHandler, SelectSock::READ, SelectSock::INF));
            }
        }
    }
    /*
    char* buffer = (char*)HeapAlloc(GetProcessHeap(), HEAP_NO_SERIALIZE, SHARED_BUFFER_SIZE);
    if(!buffer) {
        LOG_ERROR("HeapAlloc(size=%u) failed (e=%d)", SHARED_BUFFER_SIZE, GetLastError());
    }
    */
    LOG("Starting Server...");
    return server.Run(globalSharedBuffer, sizeof(globalSharedBuffer));
}