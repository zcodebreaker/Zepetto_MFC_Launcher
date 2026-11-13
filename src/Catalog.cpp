#include "Catalog.h"
#include "LauncherError.h"
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cwctype>

namespace pb
{
Bytes readBytes(const fs::path &path, uint64_t limit)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto n = in.tellg();
    if (!in || n <= 0 || static_cast<uint64_t>(n) > limit)
        throw std::runtime_error("Arquivo ausente ou com tamanho inválido: " +
                                 utf8(path.filename().wstring()));
    Bytes data(static_cast<size_t>(n));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char *>(data.data()), n))
        throw std::runtime_error("Falha de leitura: " + utf8(path.filename().wstring()));
    return data;
}
void decodeRotation(Bytes &bytes, unsigned shift)
{
    unsigned previous = bytes.back();
    for (auto &value : bytes)
    {
        unsigned current = value;
        value = static_cast<unsigned char>((previous << shift) | (current >> (8 - shift)));
        previous = current;
    }
}
fs::path relativeFile(const std::wstring &text)
{
    fs::path input(text), out;
    if (text.empty() || input.has_root_path() || text.find_first_of(L":*?<>\"|") != text.npos ||
        text.find_first_of(std::wstring(1, L'\0')) != text.npos)
        throw std::runtime_error("Caminho de arquivo inválido.");
    for (const auto &part : input)
    {
        auto name = part.wstring();
        if (name == L".")
            continue;
        if (name.empty() || name == L".." || name.back() == L'.' || name.back() == L' ' ||
            std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }))
            throw std::runtime_error("Componente de caminho inválido.");
        auto stem = name.substr(0, name.find(L'.'));
        std::transform(stem.begin(), stem.end(), stem.begin(), ::towupper);
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" ||
            stem == L"CONOUT$" ||
            (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
             (iswdigit(stem[3]) || stem[3] == L'¹' || stem[3] == L'²' || stem[3] == L'³')))
            throw std::runtime_error("Nome reservado no catálogo.");
        out /= part;
    }
    if (out.empty())
        throw std::runtime_error("Caminho de arquivo vazio.");
    return out;
}
fs::path insideRoot(const fs::path &root, const fs::path &relative)
{
    fs::path dest = root;
    for (auto &part : relativeFile(relative.wstring()))
    {
        dest /= part;
        DWORD attr = GetFileAttributesW(dest.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("O caminho atravessa um link: " + utf8(dest.wstring()));
    }
    return dest;
}
static std::wstring ansi(const std::string &s)
{
    int n = MultiByteToWideChar(1252, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(1252, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
static uint32_t word(std::istream &in)
{
    uint64_t value;
    if (!(in >> value) || value > UINT32_MAX)
        throw std::runtime_error("Valor numérico inválido no catálogo.");
    return static_cast<uint32_t>(value);
}
static uint64_t date(std::istream &in)
{
    const uint64_t high = word(in);
    auto version = (high << 32) | word(in);
    stampText(version);
    return version;
}
Catalog Catalog::load(const fs::path &path)
{
    auto bytes = readBytes(path, 64ULL << 20);
    decodeRotation(bytes, 4);
    if (bytes.size() < 6 || bytes[0] < 1 || bytes[0] > 4 || bytes[1] || bytes[2] || bytes[3] ||
        bytes.back() || std::find(bytes.begin() + 4, bytes.end() - 1, 0) != bytes.end() - 1)
        throw std::runtime_error("Formato de FileList.dat inválido.");
    const auto format = bytes[0];
    std::string contents(bytes.begin() + 4, bytes.end() - 1);
    bool quoted = false;
    for (char &c : contents)
    {
        if (c == '"')
            quoted = !quoted;
        else if (!quoted && (c == ';' || c == ',' || c == '\b'))
            c = ' ';
    }
    if (quoted)
        throw std::runtime_error("Aspas incompletas no catálogo.");
    std::istringstream in(contents);
    std::string tag;
    Catalog result;
    while (in >> tag)
    {
        if (!result.version && tag == "*Header")
            result.version = date(in);
        else if (result.version && tag == "*FileInfo")
        {
            std::string name, folder;
            // Paths use literal backslashes, including directly before the closing quotation mark.
            if (!(in >> std::quoted(name, '"', '\0') >> std::quoted(folder, '"', '\0')) ||
                fs::path(ansi(name)).filename() != fs::path(ansi(name)))
                throw std::runtime_error("Nome inválido no catálogo.");
            FileEntry e;
            e.path = relativeFile((fs::path(ansi(folder)) / relativeFile(ansi(name))).wstring());
            e.catalogKey = folder + name;
            e.version = date(in);
            e.size = word(in);
            for (int i = 3; i >= 0; --i)
            {
                uint32_t value = word(in);
                std::memcpy(e.md5.data() + 4 * i, &value, 4);
            }
            if (format >= 2)
            {
                e.creation = {word(in), word(in)};
                e.modified = {word(in), word(in)};
            }
            if (format >= 3)
                e.startFlags = word(in);
            if (format >= 4)
                e.checkFlags = word(in);
            if (!result.files.emplace(e.path, e).second)
                throw std::runtime_error("Caminho duplicado no catálogo.");
        }
        else
            throw std::runtime_error("Registro desconhecido no catálogo.");
    }
    if (!result.version)
        throw std::runtime_error("Cabeçalho ausente no catálogo.");
    return result;
}
static Digest digestFile(const fs::path &path, uint64_t size, const Progress &progress)
{
    progress(0, size);
    static struct Provider
    {
        BCRYPT_ALG_HANDLE h{};
        Provider()
        {
            if (BCryptOpenAlgorithmProvider(&h, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0)
                throw std::runtime_error("MD5 indisponível.");
        }
        ~Provider()
        {
            BCryptCloseAlgorithmProvider(h, 0);
        }
    } provider;
    struct Hash
    {
        BCRYPT_HASH_HANDLE h{};
        ~Hash()
        {
            if (h)
                BCryptDestroyHash(h);
        }
    } hash;
    if (BCryptCreateHash(provider.h, &hash.h, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Falha ao iniciar MD5.");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Falha ao ler " + utf8(path.wstring()));
    std::array<unsigned char, 0xffff> block;
    uint64_t done = 0;
    while (in)
    {
        in.read(reinterpret_cast<char *>(block.data()), block.size());
        const auto n = static_cast<ULONG>(in.gcount());
        if (!n)
            break;
        done += n;
        progress(done, size);
        if (BCryptHashData(hash.h, block.data(), n, 0) < 0)
            throw std::runtime_error("Falha ao verificar MD5.");
    }
    Digest out;
    if (in.bad() || done != size || BCryptFinishHash(hash.h, out.data(), out.size(), 0) < 0)
        throw std::runtime_error("Arquivo alterado durante a leitura: " + utf8(path.wstring()));
    return out;
}
static void applyDates(const fs::path &path, const FILETIME *creation, const FILETIME *modified)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = f != INVALID_HANDLE_VALUE && SetFileTime(f, creation, nullptr, modified);
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    if (!ok)
        throw std::runtime_error("Falha ao gravar a data de " + utf8(path.wstring()));
}
// FileList timestamps are UTC. The legacy file checker stores the local-time representation
// on non-NTFS volumes and accepts a two-second difference on those volumes.
bool isNtfs(const fs::path &path)
{
    wchar_t volume[MAX_PATH]{}, type[32]{};
    if (!GetVolumePathNameW(path.c_str(), volume, MAX_PATH) ||
        !GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, nullptr, type, ARRAYSIZE(type)))
        throw LauncherError(ErrorCode::CannotGetFilesystemName, "Sistema de arquivos indisponível.");
    return !_wcsicmp(type, L"NTFS");
}
static FILETIME storageTime(FILETIME time, bool ntfs)
{
    if (ntfs || (!time.dwHighDateTime && !time.dwLowDateTime))
        return time;
    FILETIME local{};
    if (!FileTimeToLocalFileTime(&time, &local))
        throw std::runtime_error("Data de arquivo inválida.");
    return local;
}
void setFileDates(const fs::path &path, const FileEntry &file, bool ntfs)
{
    auto creation = storageTime(file.creation, ntfs), modified = storageTime(file.modified, ntfs);
    applyDates(path, &creation, &modified);
}
static bool sameModifiedTime(const FILETIME &actual, const FILETIME &expected, bool ntfs)
{
    if (ntfs)
        return CompareFileTime(&actual, &expected) == 0;
    FILETIME utc{};
    if (!LocalFileTimeToFileTime(&actual, &utc))
        return false;
    uint64_t a = (uint64_t(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
    uint64_t b = (uint64_t(expected.dwHighDateTime) << 32) | expected.dwLowDateTime;
    return (a > b ? a - b : b - a) / 10000 <= 2000;
}
unsigned fileDifference(const fs::path &path, const FileEntry &file, uint32_t flags, bool ntfs, const Progress &progress)
{
    if (!flags)
        return 0;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return 1;
    uint64_t size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if ((flags & 2) && size != file.size)
        return 2;
    if (flags & 4)
    {
        try
        {
            if (digestFile(path, size, progress) != file.md5)
                return 4;
        }
        catch (const std::runtime_error &)
        {
            return 4;
        }
    }
    if (flags & 8)
    {
        if (sameModifiedTime(data.ftLastWriteTime, file.modified, ntfs))
            return 0;
        if (!(flags & 4))
            return 8;
        try
        {
            auto modified = storageTime(file.modified, ntfs);
            applyDates(path, nullptr, &modified);
        }
        catch (const std::runtime_error &)
        {
            return 8;
        }
    }
    return 0;
}
void replaceFile(const fs::path &source, const fs::path &destination)
{
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Não foi possível substituir " + utf8(destination.wstring()) + " (" +
                                 std::to_string(GetLastError()) + ").");
}
} // namespace pb
