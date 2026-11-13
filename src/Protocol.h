#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pb
{
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
constexpr uint16_t hello = 1536, versionRequest = 1538, versionReply = 1539, errorReport = 1540, errorAck = 1541;
struct Frame
{
    uint16_t opcode;
    Bytes data;
};
struct Release
{
    uint32_t launcher;
    uint64_t stamp;
    std::string base;
    uint64_t gameArgument = 0;
};
struct Winsock
{
    Winsock();
    ~Winsock()
    {
        WSACleanup();
    }
};
struct Socket
{
    SOCKET fd = INVALID_SOCKET;
    explicit Socket(SOCKET s = INVALID_SOCKET) : fd(s) {}
    Socket(Socket &&other) noexcept : fd(std::exchange(other.fd, INVALID_SOCKET)) {}
    void reset(SOCKET replacement = INVALID_SOCKET)
    {
        if (fd != INVALID_SOCKET)
            closesocket(fd);
        fd = replacement;
    }
    ~Socket() { reset(); }
    Socket(const Socket &) = delete;
};
std::wstring wide(const std::string &s);
std::string utf8(const std::wstring &s);
std::wstring setting(const fs::path &file, const wchar_t *section, const wchar_t *key,
                     const wchar_t *fallback);
int portNumber(const std::wstring &value);
fs::path modulePath();
fs::path appFolder();
std::string stampText(uint64_t stamp);
uint64_t parseStamp(const std::string &text);
void timeouts(SOCKET s, DWORD timeout);
void sendFrame(SOCKET s, uint16_t opcode, const Bytes &data);
Frame readFrame(SOCKET s, const std::function<void()> &checkpoint, ULONGLONG deadline);
Bytes receiveFrame(SOCKET s, uint16_t opcode, const std::function<void()> &checkpoint = {},
                   ULONGLONG deadline = 0);
Bytes releasePayload(const Release &version);

} // namespace pb
