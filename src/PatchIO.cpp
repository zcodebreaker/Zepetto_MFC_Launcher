#include "PatchIO.h"
#include "LauncherError.h"
#include <iphlpapi.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <miniz.h>
#include <algorithm>
#include <fstream>
#include <memory>

namespace pb
{
using Microsoft::WRL::ComPtr;
struct Xml
{
    std::wstring element;
    std::map<std::wstring, std::wstring> values;
    LauncherSet set;
};
static int number(const std::wstring &text, int max)
{
    if (text.empty() || text.size() > 9 || text.find_first_not_of(L"0123456789") != text.npos)
        throw std::runtime_error("Número inválido na configuração do launcher.");
    int n = std::stoi(text);
    if (n > max)
        throw std::runtime_error("Valor de configuração fora do intervalo.");
    return n;
}
static Xml readXml(const fs::path &path, bool rotated)
{
    auto data = readBytes(path, 4 << 20);
    if (rotated)
        decodeRotation(data, 1);
    ComPtr<IStream> input;
    input.Attach(SHCreateMemStream(data.data(), static_cast<UINT>(data.size())));
    ComPtr<IXmlReader> reader;
    if (!input || FAILED(CreateXmlReader(IID_PPV_ARGS(&reader), nullptr)) ||
        FAILED(reader->SetInput(input.Get())))
        throw std::runtime_error("Falha ao abrir XML do launcher.");
    reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    Xml out;
    XmlNodeType type;
    HRESULT hr;
    std::wstring field;
    while ((hr = reader->Read(&type)) == S_OK)
    {
        UINT depth = 0;
        reader->GetDepth(&depth);
        const wchar_t *value = L"";
        if (type == XmlNodeType_Element)
        {
            reader->GetLocalName(&value, nullptr);
            std::wstring name(value);
            if (depth == 0)
                out.element = name;
            if (depth == 1)
                field = name;
            std::wstring component, version;
            if (reader->MoveToFirstAttribute() == S_OK)
            {
                do
                {
                    reader->GetLocalName(&value, nullptr);
                    std::wstring attr(value);
                    reader->GetValue(&value, nullptr);
                    if (attr == L"name")
                        component = value;
                    if (attr == L"version")
                        version = value;
                } while (reader->MoveToNextAttribute() == S_OK);
                reader->MoveToElement();
            }
            if (!depth)
                out.set.version = number(version, 999999999);
            if (depth == 1 && name == L"file" &&
                !out.set.components.emplace(component, number(version, 999999999)).second)
                throw std::runtime_error("Componente duplicado em Launcher.svl.");
        }
        else if (depth == 2 && (type == XmlNodeType_Text || type == XmlNodeType_CDATA))
        {
            reader->GetValue(&value, nullptr);
            out.values[field] += value;
        }
    }
    if (FAILED(hr) || out.element.empty())
        throw std::runtime_error("XML inválido: " + utf8(path.filename().wstring()));
    return out;
}
uint32_t xmlVersion(const fs::path &path, const std::wstring &element, bool rotated)
{
    auto xml = readXml(path, rotated);
    if (xml.element != element)
        throw std::runtime_error("Tipo de documento incompatível.");
    return xml.set.version;
}
std::map<std::wstring, std::wstring> loadTexts(const fs::path &path, const wchar_t *root)
{
    if (!fs::exists(path))
        return {};
    auto xml = readXml(path, false);
    if (xml.element != root)
        throw std::runtime_error("Documento de textos incompatível.");
    return std::move(xml.values);
}
std::wstring localizedText(const std::map<std::wstring, std::wstring> &texts, const wchar_t *key,
                           const std::wstring &fallback)
{
    const auto found = texts.find(key);
    return found != texts.end() && !found->second.empty() ? found->second : fallback;
}
LauncherSet loadSet(const fs::path &path)
{
    auto xml = readXml(path, true);
    if (xml.element != L"launcherset")
        throw std::runtime_error("Launcher.svl inválido.");
    for (auto key : {L"Launcher", L"config.zpt", L"Messages.xml", L"UI.xml"})
        if (!xml.set.components.count(key))
            throw std::runtime_error("Conjunto do launcher incompleto.");
    return xml.set;
}
ClientConfig loadConfig(const fs::path &folder)
{
    auto xml = readXml(folder / L"config.zpt", true);
    if (xml.element != L"config")
        throw std::runtime_error("config.zpt inválido.");
    ClientConfig config;
    config.newsUrl = xml.values[L"web_url"];
    config.hideNews = xml.values[L"notice_ui"] == L"hide";
    config.host = xml.values[L"server_address"];
    config.port = portNumber(xml.values[L"server_port"]);
    config.gameExe = relativeFile(xml.values[L"executable_file"]);
    if (config.gameExe.has_parent_path())
        throw std::runtime_error("executable_file deve indicar um arquivo ao lado de PBLauncher.exe.");
    if (xml.values.count(L"patcherror_retry_count"))
        config.retries = number(xml.values.at(L"patcherror_retry_count"), 100);
    if (xml.values.count(L"server_ack_timeout_sec"))
        config.timeout = std::max(1, number(xml.values.at(L"server_ack_timeout_sec"), 600));
    if (config.host.empty() || _wcsicmp(config.gameExe.extension().c_str(), L".exe"))
        throw std::runtime_error("Endereço do servidor ou executável do jogo inválido.");
    return config;
}
static Socket connectVersion(const ClientConfig &c, const std::function<void()> &checkpoint)
{
    ADDRINFOW hints{}, *raw = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (GetAddrInfoW(c.host.c_str(), std::to_wstring(c.port).c_str(), &hints, &raw))
        throw LauncherError(ErrorCode::ConvertDomainToIpFailed,
                            "Endereço do servidor de atualização indisponível.");
    std::unique_ptr<ADDRINFOW, decltype(&FreeAddrInfoW)> addresses(raw, FreeAddrInfoW);
    for (auto a = raw; a; a = a->ai_next)
    {
        checkpoint();
        Socket s(socket(a->ai_family, SOCK_STREAM, IPPROTO_TCP));
        if (s.fd == INVALID_SOCKET)
            continue;
        u_long nonblock = 1;
        ioctlsocket(s.fd, FIONBIO, &nonblock);
        int result = connect(s.fd, a->ai_addr, static_cast<int>(a->ai_addrlen));
        if (result && WSAGetLastError() != WSAEWOULDBLOCK)
            continue;
        int error = 0, len = sizeof(error);
        if (result)
        {
            ULONGLONG deadline = GetTickCount64() + c.timeout * 1000ULL;
            WSAPOLLFD fd{s.fd, POLLWRNORM, 0};
            do
            {
                checkpoint();
                result = WSAPoll(&fd, 1, 100);
            } while (!result && GetTickCount64() < deadline);
            if (result <= 0 ||
                getsockopt(s.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &len) || error)
                continue;
        }
        nonblock = 0;
        ioctlsocket(s.fd, FIONBIO, &nonblock);
        timeouts(s.fd, c.timeout * 1000);
        receiveFrame(s.fd, hello, checkpoint, GetTickCount64() + c.timeout * 1000ULL);
        return s;
    }
    throw LauncherError(ErrorCode::ConnectFail2, "Não foi possível conectar ao servidor de atualização.");
}
static Bytes versionPayload(uint32_t version)
{
    Bytes payload(12);
    std::memcpy(payload.data(), &version, 4);
    ULONG size = 0;
    GetAdaptersInfo(nullptr, &size);
    Bytes storage(size);
    auto adapter = reinterpret_cast<IP_ADAPTER_INFO *>(storage.data());
    if (size && GetAdaptersInfo(adapter, &size) == NO_ERROR)
        std::memcpy(payload.data() + 4, adapter->Address, std::min<UINT>(8, adapter->AddressLength));
    return payload;
}
static Release parseRelease(const Bytes &b)
{
    if (b.size() < 21 || !b[12] || b[12] > 128 || b.size() != 21u + b[12])
        throw std::runtime_error("Resposta de versão inválida.");
    Release release{};
    std::memcpy(&release.launcher, b.data(), 4);
    std::memcpy(&release.stamp, b.data() + 4, 8);
    release.base.assign(b.begin() + 13, b.begin() + 13 + b[12]);
    std::memcpy(&release.gameArgument, b.data() + 13 + b[12], 8);
    stampText(release.stamp);
    return release;
}
Release VersionSession::query(const ClientConfig &c, uint32_t version, const std::function<void()> &checkpoint,
                              const Bytes &report)
{
    if (host != c.host || port != c.port)
    {
        connection.reset();
        host = c.host;
        port = c.port;
    }
    const auto request = versionPayload(version);
    Release release;
    for (;;)
    {
        checkpoint();
        const bool reused = connection.fd != INVALID_SOCKET;
        if (!reused)
        {
            auto fresh = connectVersion(c, checkpoint);
            connection.reset(std::exchange(fresh.fd, INVALID_SOCKET));
        }
        try
        {
            timeouts(connection.fd, c.timeout * 1000);
            sendFrame(connection.fd, versionRequest, request);
            release = parseRelease(receiveFrame(connection.fd, versionReply, checkpoint,
                                                GetTickCount64() + c.timeout * 1000ULL));
            break;
        }
        catch (const std::runtime_error &)
        {
            connection.reset();
            if (!reused)
                throw;
        }
    }
    if (!report.empty())
    {
        sendFrame(connection.fd, errorReport, report);
        receiveFrame(connection.fd, errorAck, checkpoint, GetTickCount64() + 3000);
    }
    return release;
}
struct Internet
{
    HINTERNET h;
    explicit Internet(HINTERNET value) : h(value)
    {
        if (!h)
            throw std::runtime_error("Falha HTTP (" + std::to_string(GetLastError()) + ").");
    }
    ~Internet()
    {
        WinHttpCloseHandle(h);
    }
};
static std::wstring urlPart(const fs::path &remote)
{
    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring out;
    for (unsigned char c : utf8(remote.generic_wstring()))
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-._~/", c))
            out += wchar_t(c);
        else
        {
            out += L'%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
void fetchZip(const std::string &base, const fs::path &remote, const fs::path &archive, uint64_t limit,
              const Progress &progress)
{
    auto url = wide(base);
    if (url.find_first_of(L"?#") != url.npos || url.find(L'\0') != url.npos)
        throw std::runtime_error("URL de patch inválida.");
    if (url.empty() || url.back() != L'/')
        url += L'/';
    url += urlPart(remote);
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, ICU_REJECT_USERPWD, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS))
        throw std::runtime_error("URL de patch inválida.");
    Internet session(WinHttpOpen(L"LauncherMFC", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
    WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 5000);
    Internet host(WinHttpConnect(session.h, std::wstring(parts.lpszHostName, parts.dwHostNameLength).c_str(),
                                 parts.nPort, 0));
    Internet request(WinHttpOpenRequest(
        host.h, L"GET", parts.lpszUrlPath, nullptr, nullptr, nullptr,
        WINHTTP_FLAG_ESCAPE_DISABLE | (parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)));
    std::error_code ec;
    uint64_t offset = fs::file_size(archive, ec);
    if (ec || offset > limit)
        offset = 0;
    auto range = L"Range: bytes=" + std::to_wstring(offset) + L"-";
    if (!WinHttpSendRequest(request.h, offset ? range.c_str() : nullptr, offset ? DWORD(-1) : 0, nullptr, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        throw std::runtime_error("Falha ao baixar " + utf8(remote.wstring()) + " (" +
                                 std::to_string(GetLastError()) + ").");
    DWORD status = 0, n = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status,
                        &n, nullptr);
    if (status == 206 && offset)
    {
        wchar_t contentRange[96]{};
        n = sizeof(contentRange);
        unsigned long long begin{}, end{}, complete{};
        if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_RANGE, nullptr, contentRange, &n,
                                 nullptr) ||
            swscanf_s(contentRange, L"bytes %llu-%llu/%llu", &begin, &end, &complete) != 3 ||
            begin != offset || end < begin || complete > limit || end != complete - 1)
        {
            fs::remove(archive);
            throw std::runtime_error("Intervalo inválido na retomada do download.");
        }
    }
    else if (status == 200)
        offset = 0;
    else
    {
        if (status == 416 && offset)
            fs::remove(archive);
        throw std::runtime_error("HTTP " + std::to_string(status) + " ao baixar " + utf8(remote.wstring()));
    }
    wchar_t length[32]{};
    n = sizeof(length);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, length, &n, nullptr))
    {
        auto remaining = std::stoull(length);
        if (remaining > limit - offset)
            throw std::runtime_error("Pacote maior que o esperado.");
        total = offset + remaining;
    }
    if (total > limit)
        throw std::runtime_error("Pacote maior que o esperado.");
    if (total && fs::space(archive.parent_path()).available < total - offset)
        throw LauncherError(ErrorCode::DiskFreeLack, "Espaço livre insuficiente para baixar o patch.");
    std::ofstream out(archive, std::ios::binary | (offset ? std::ios::app : std::ios::trunc));
    std::array<char, 65536> block;
    uint64_t done = offset;
    for (;;)
    {
        progress(done, total);
        if (!WinHttpReadData(request.h, block.data(), static_cast<DWORD>(block.size()), &n))
            throw std::runtime_error("Download interrompido.");
        if (!n)
            break;
        done += n;
        if (done > limit || !out.write(block.data(), n))
            throw std::runtime_error("Falha ao gravar pacote de patch.");
    }
    out.close();
    if (!out || !done || (total && done != total))
        throw std::runtime_error("Download de patch incompleto.");
}
void extractZip(const fs::path &archive, const fs::path &name, const fs::path &output, uint64_t limit,
                const Progress &progress)
{
    using File = std::unique_ptr<FILE, decltype(&fclose)>;
    File input(_wfopen(archive.c_str(), L"rb"), fclose);
    mz_zip_archive zip{};
    if (!input || !mz_zip_reader_init_cfile(&zip, input.get(), 0, 0))
        throw std::runtime_error("ZIP inválido: " + utf8(name.wstring()));
    struct End
    {
        mz_zip_archive &zip;
        ~End()
        {
            mz_zip_reader_end(&zip);
        }
    } end{zip};
    if (mz_zip_reader_get_num_files(&zip) != 1)
        throw std::runtime_error("O ZIP deve conter exatamente o arquivo publicado.");
    mz_zip_archive_file_stat file{};
    if (!mz_zip_reader_file_stat(&zip, 0, &file) || file.m_is_directory || file.m_uncomp_size > limit ||
        _stricmp(file.m_filename, utf8(name.filename().wstring()).c_str()))
        throw std::runtime_error("Conteúdo inesperado no ZIP: " + utf8(name.wstring()));
    if (fs::space(output.parent_path()).available < file.m_uncomp_size)
        throw LauncherError(ErrorCode::DiskFreeLack, "Espaço livre insuficiente para extrair o patch.");
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    struct Sink
    {
        std::ofstream &out;
        const Progress &progress;
        uint64_t total;
        std::exception_ptr error;
    };
    Sink sink{out, progress, file.m_uncomp_size};
    auto write = [](void *state, mz_uint64 off, const void *data, size_t n) -> size_t {
        auto &s = *static_cast<Sink *>(state);
        try
        {
            s.progress(off + n, s.total);
            if (s.out.write(static_cast<const char *>(data), n))
                return n;
        }
        catch (...)
        {
            s.error = std::current_exception();
        }
        return 0;
    };
    const auto ok = mz_zip_reader_extract_to_callback(&zip, 0, write, &sink, 0);
    if (sink.error)
        std::rethrow_exception(sink.error);
    out.close();
    if (!ok || !out)
        throw std::runtime_error("Falha ao extrair " + utf8(name.wstring()));
}
Stage::Stage(const fs::path &root, const wchar_t *folder)
{
    GUID guid;
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("Falha ao preparar diretório temporário.");
    wchar_t id[40]{};
    StringFromGUID2(guid, id, 40);
    path = insideRoot(root, fs::path(folder) / id);
    fs::create_directories(path.parent_path());
    if (!fs::create_directory(path))
        throw std::runtime_error("Diretório temporário indisponível.");
}
Stage::~Stage()
{
    std::error_code ec;
    fs::remove_all(path, ec);
}
} // namespace pb
