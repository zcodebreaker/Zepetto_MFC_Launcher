#include "Protocol.h"
#include "LauncherError.h"

namespace pb
{
Winsock::Winsock()
{
    WSADATA w{};
    if (WSAStartup(MAKEWORD(2, 2), &w))
        throw LauncherError(ErrorCode::SocketsInitFailed, "Falha ao iniciar Winsock.");
}
std::wstring wide(const std::string &s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (!n)
        throw std::runtime_error("Texto UTF-8 inválido.");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}
std::string utf8(const std::wstring &s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}
fs::path modulePath()
{
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(nullptr, path, _countof(path));
    if (!n || n == _countof(path))
        throw LauncherError(ErrorCode::CannotGetModuleName, "Caminho do executável indisponível.");
    return fs::path(path);
}
fs::path appFolder()
{
    return modulePath().parent_path();
}
std::wstring setting(const fs::path &file, const wchar_t *section, const wchar_t *key,
                     const wchar_t *fallback)
{
    wchar_t text[4096];
    DWORD n = GetPrivateProfileStringW(section, key, fallback, text, _countof(text), file.c_str());
    if (n == _countof(text) - 1)
        throw std::runtime_error("Valor de configuração longo demais.");
    return text;
}
int portNumber(const std::wstring &value)
{
    if (value.empty() || value.size() > 5 || value.find_first_not_of(L"0123456789") != value.npos)
        throw std::runtime_error("Porta inválida.");
    int n = std::stoi(value);
    if (!n || n > 65535)
        throw std::runtime_error("Porta inválida.");
    return n;
}
std::string stampText(uint64_t stamp)
{
    auto s = std::to_string(stamp);
    if (s.size() != 14)
        throw std::runtime_error("Versão de patch inválida.");
    return s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2) + "_" + s.substr(8, 2) + "-" +
           s.substr(10, 2) + "-" + s.substr(12, 2);
}
uint64_t parseStamp(const std::string &text)
{
    std::string digits;
    for (char c : text)
        if (c >= '0' && c <= '9')
            digits += c;
    if (digits.size() != 14 || stampText(std::stoull(digits)) != text)
        throw std::runtime_error("Data inválida em Version.ini.");
    return std::stoull(digits);
}
void timeouts(SOCKET s, DWORD timeout)
{
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
}
template <class T> static void append(Bytes &b, T value)
{
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        b.push_back((unsigned char)value);
        value >>= 8;
    }
}
template <class T> static T read(const Bytes &b, size_t off)
{
    if (off > b.size() || sizeof(T) > b.size() - off)
        throw std::runtime_error("Resposta de versão incompleta.");
    T n = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        n |= T(b[off + i]) << (8 * i);
    return n;
}
void sendFrame(SOCKET s, uint16_t op, const Bytes &data)
{
    if (data.size() > 1024)
        throw std::runtime_error("Mensagem longa demais.");
    Bytes b;
    append(b, (uint16_t)data.size());
    append(b, op);
    b.insert(b.end(), data.begin(), data.end());
    size_t off = 0;
    while (off < b.size())
    {
        int n = send(s, (const char *)b.data() + off, (int)(b.size() - off), 0);
        if (n <= 0)
            throw std::runtime_error("Falha ao enviar ao servidor de atualização.");
        off += n;
    }
}
static Bytes receive(SOCKET s, size_t count, const std::function<void()> &checkpoint, ULONGLONG deadline)
{
    Bytes b(count);
    size_t off = 0;
    while (off < count)
    {
        if (checkpoint || deadline)
        {
            if (checkpoint)
                checkpoint();
            if (deadline && GetTickCount64() >= deadline)
                throw LauncherError(ErrorCode::ServerAckTimeout, "Tempo limite da resposta do servidor excedido.");
            WSAPOLLFD poll{s, POLLRDNORM, 0};
            int ready = WSAPoll(&poll, 1, 100);
            if (!ready)
                continue;
            if (ready < 0)
                throw std::runtime_error("Falha na leitura do servidor de atualização.");
        }
        int n = recv(s, (char *)b.data() + off, (int)(count - off), 0);
        if (n <= 0)
            throw std::runtime_error("Servidor de atualização desconectado ou sem resposta.");
        off += n;
    }
    return b;
}
Frame readFrame(SOCKET s, const std::function<void()> &checkpoint, ULONGLONG deadline)
{
    auto h = receive(s, 4, checkpoint, deadline);
    auto size = read<uint16_t>(h, 0);
    if (size > 1024)
        throw std::runtime_error("Protocolo do servidor incompatível.");
    return {read<uint16_t>(h, 2), receive(s, size, checkpoint, deadline)};
}
Bytes receiveFrame(SOCKET s, uint16_t opcode, const std::function<void()> &checkpoint, ULONGLONG deadline)
{
    auto frame = readFrame(s, checkpoint, deadline);
    if (frame.opcode != opcode)
        throw std::runtime_error("Protocolo do servidor incompatível.");
    return std::move(frame.data);
}
Bytes releasePayload(const Release &v)
{
    if (v.base.empty() || v.base.size() > 128)
        throw std::runtime_error("URL de patch deve ter entre 1 e 128 bytes.");
    Bytes b;
    append(b, v.launcher);
    append(b, v.stamp);
    b.push_back((unsigned char)v.base.size());
    b.insert(b.end(), v.base.begin(), v.base.end());
    append(b, v.gameArgument);
    return b;
}
} // namespace pb
