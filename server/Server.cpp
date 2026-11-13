#include "Protocol.h"
#include <httplib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace pb;
static Release loadRelease(const fs::path &dir)
{
    std::ifstream in(dir / "Version.ini");
    std::string launcher, date, url;
    if (!std::getline(in, launcher) || !std::getline(in, date) || !std::getline(in, url) ||
        launcher.empty() || launcher.size() > 9 ||
        launcher.find_first_not_of("0123456789") != launcher.npos ||
        (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0))
        throw std::runtime_error("Version.ini inválido.");
    return {(uint32_t)std::stoul(launcher), parseStamp(date), url};
}
static void logPacket(const std::string &peer, const std::string &message)
{
    static std::mutex mutex;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::lock_guard guard(mutex);
    std::cout << std::setfill('0') << std::setw(2) << now.wHour << ':' << std::setw(2) << now.wMinute << ':'
              << std::setw(2) << now.wSecond << " [" << peer << "] " << message << std::endl;
}
static void serveClient(Socket client, const std::string &peer, const fs::path &dir, const std::atomic<bool> &stopping)
{
    logPacket(peer, "conectado");
    try
    {
        timeouts(client.fd, 5000);
        sendFrame(client.fd, hello, {});
        bool negotiated = false;
        const auto initialDeadline = GetTickCount64() + 5000;
        while (!stopping)
        {
            WSAPOLLFD poll{client.fd, POLLRDNORM, 0};
            int ready = WSAPoll(&poll, 1, 1000);
            if (ready == SOCKET_ERROR)
                throw std::runtime_error("Falha na leitura TCP.");
            if (!ready)
            {
                if (!negotiated && GetTickCount64() >= initialDeadline)
                    break;
                continue;
            }
            char first;
            if (recv(client.fd, &first, 1, MSG_PEEK) <= 0)
                break;
            auto frame = readFrame(client.fd, {}, GetTickCount64() + 5000);
            std::ostringstream message;
            message << "RX " << frame.opcode << " (" << frame.data.size() << " bytes)";
            if (frame.opcode == versionRequest && frame.data.size() == 12)
            {
                uint32_t version;
                std::memcpy(&version, frame.data.data(), sizeof(version));
                message << " versao=" << version;
                logPacket(peer, message.str());
                sendFrame(client.fd, versionReply, releasePayload(loadRelease(dir)));
                negotiated = true;
            }
            else if (negotiated && frame.opcode == errorReport && frame.data.size() == 8)
            {
                uint32_t state, code;
                std::memcpy(&state, frame.data.data(), 4);
                std::memcpy(&code, frame.data.data() + 4, 4);
                message << " estado=" << state << " erro=0x" << std::hex << std::uppercase << code;
                logPacket(peer, message.str());
                sendFrame(client.fd, errorAck, {});
            }
            else
            {
                logPacket(peer, message.str() + " pacote invalido");
                break;
            }
        }
    }
    catch (const std::exception &e)
    {
        logPacket(peer, e.what());
    }
    logPacket(peer, "desconectado");
}
int main()
try
{
    SetConsoleTitleW(L"Update");
    Winsock winsock;
    const auto dir = appFolder(), ini = dir / "Config.ini";
    auto bindIp = setting(ini, L"Address", L"Ip0", L"127.0.0.1");
    int port = portNumber(setting(ini, L"Address", L"Port0", L"49100"));
    releasePayload(loadRelease(dir));
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    BOOL exclusive = TRUE;
    setsockopt(listener.fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&exclusive, sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (InetPtonW(AF_INET, bindIp.c_str(), &addr.sin_addr) != 1 ||
        bind(listener.fd, (sockaddr *)&addr, sizeof(addr)) || listen(listener.fd, SOMAXCONN))
        throw std::runtime_error("Falha ao abrir o endereço TCP de Config.ini.");
    httplib::Server http;
    std::thread web;
    if (setting(ini, L"HTTP", L"Enabled", L"1") == L"1")
    {
        auto root = dir / setting(ini, L"HTTP", L"Directory", L"patch");
        int httpPort = portNumber(setting(ini, L"HTTP", L"Port", L"49101"));
        http.new_task_queue = [] { return new httplib::ThreadPool(4, 64); };
        if (!http.set_mount_point("/patch", utf8(root.wstring())) ||
            !http.bind_to_port(utf8(bindIp), httpPort))
            throw std::runtime_error("Diretório de patches ou porta HTTP indisponível.");
        web = std::thread([&http] { http.listen_after_bind(); });
        std::cout << "HTTP /patch: " << root << " (" << httpPort << ")\n";
    }
    std::cout << "UpdateServerss TCP " << utf8(bindIp) << ":" << port << std::endl;
    std::atomic<bool> stopping{false};
    std::vector<std::future<void>> clients;
    for (;;)
    {
        sockaddr_in address{};
        int length = sizeof(address);
        Socket client(accept(listener.fd, reinterpret_cast<sockaddr *>(&address), &length));
        if (client.fd == INVALID_SOCKET)
            break;
        char ip[INET_ADDRSTRLEN]{};
        InetNtopA(AF_INET, &address.sin_addr, ip, sizeof(ip));
        auto peer = std::string(ip) + ":" + std::to_string(ntohs(address.sin_port));
        clients.erase(std::remove_if(clients.begin(), clients.end(), [](auto &task) {
            return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }), clients.end());
        if (clients.size() >= 128)
        {
            logPacket(peer, "limite de conexoes simultaneas atingido");
            continue;
        }
        try
        {
            clients.push_back(std::async(std::launch::async, serveClient, std::move(client), peer,
                                         std::cref(dir), std::cref(stopping)));
        }
        catch (const std::system_error &e)
        {
            logPacket(peer, e.what());
        }
    }
    stopping = true;
    clients.clear();
    http.stop();
    if (web.joinable())
        web.join();
    return 0;
}
catch (const std::exception &e)
{
    std::cerr << e.what() << std::endl;
    return 1;
}
