#include "Engine.h"
#include "NativeVersion.h"
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>

using namespace pb;
namespace
{
struct Cancelled
{
};
struct Component
{
    const wchar_t *name, *prefix, *extension, *tag;
    bool rotated;
};
constexpr Component components[] = {{L"config.zpt", L"config", L".zpt", L"config", true},
                                    {L"Messages.xml", L"Messages", L".xml", L"message", false},
                                    {L"UI.xml", L"UI", L".xml", L"ui", false}};
bool installedComponent(const fs::path &dir, const Component &c, const LauncherSet &expected)
{
    auto path = insideRoot(dir, c.name);
    if (!fs::is_regular_file(path))
        return false;
    try
    {
        return xmlVersion(path, c.tag, c.rotated) == expected.components.at(c.name);
    }
    catch (const std::runtime_error &)
    {
        return false;
    }
}
bool reserved(const fs::path &p)
{
    static const std::set<fs::path, PathOrder> owned{L"LauncherMFC.exe",
                                                     L"LauncherMFC.next.exe",
                                                     L"LauncherMFC.previous.exe",
                                                     L"LauncherMaintenance.exe",
                                                     launcherExecutable,
                                                     resourceCheckExecutable,
                                                     stagedLauncher,
                                                     previousLauncher,
                                                     L"Launcher.ini", // Ignore this retired file in older patch catalogs.
                                                     L"UserFileList.dat",
                                                     L"FileList.dat",
                                                     L"UpdaterModifier.exe",
                                                     L"Launcher.svl",
                                                     L"config.zpt",
                                                     L"Messages.xml",
                                                     L"UI.xml",
                                                     L"_DownloadPatchFiles",
                                                     L"_LauncherPatchFiles"};
    static const auto executable = modulePath().filename();
    return !p.empty() && (owned.count(*p.begin()) || !_wcsicmp(p.begin()->c_str(), executable.c_str()));
}
bool protectedOldFile(const fs::path &p)
{
    if (reserved(p))
        return true;
    for (auto &component : p)
        if (!_wcsicmp(component.c_str(), L"HShield"))
            return true;
    return false;
}
bool catalogOrder(const FileEntry *a, const FileEntry *b)
{
    return a->catalogKey < b->catalogKey;
}
uint32_t manualCheckFlags(const FileEntry &entry)
{
    if (!entry.checkFlags || *entry.path.begin() != L"HShield")
        return entry.checkFlags;
    // These components are distributed by the game patcher. HShield maintains the rest of its folder.
    const auto &name = entry.path.native();
    if (name.find(L"HSUpdate.env") != name.npos)
        return 15;
    for (const auto *component : {L"AhnUpCtl", L"AhnUpGS", L"HSInst", L"HSUpdate", L"V3Hunt", L"V3InetGS"})
        if (name.find(component) != name.npos)
            return 7;
    return 0;
}
bool gameAdministrator()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token))
        return false;
    TOKEN_ELEVATION_TYPE type{};
    DWORD size = 0;
    bool ok = GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size) != FALSE;
    BOOL member = FALSE;
    if (ok && type == TokenElevationTypeLimited)
    {
        TOKEN_LINKED_TOKEN linked{};
        if (GetTokenInformation(token, TokenLinkedToken, &linked, sizeof(linked), &size))
        {
            alignas(DWORD) BYTE sid[SECURITY_MAX_SID_SIZE]{};
            DWORD sidSize = sizeof(sid);
            if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid, &sidSize))
                CheckTokenMembership(linked.LinkedToken, sid, &member);
            CloseHandle(linked.LinkedToken);
        }
    }
    else if (ok)
        member = IsUserAnAdmin();
    CloseHandle(token);
    return member != FALSE;
}
void startProcess(const fs::path &exe, const std::wstring &args, const fs::path &cwd, const wchar_t *verb)
{
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = verb;
    info.lpFile = exe.c_str();
    info.lpDirectory = cwd.c_str();
    info.lpParameters = args.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!fs::is_regular_file(exe) || !ShellExecuteExW(&info))
        throw std::runtime_error("Não foi possível abrir " + utf8(exe.filename().wstring()) + ".");
}
} // namespace
Engine::Engine(fs::path directory, std::map<std::wstring, std::wstring> initialMessages)
    : root(std::move(directory)), appDir(appFolder()), messages(std::move(initialMessages))
{
}
void Engine::transition(State next)
{
    if (next == state)
        return;
    static constexpr const char *names[] = {"UNKNOWN",
                                            "START",
                                            "PRE_CONNECT",
                                            "PRE_UPDATER_VERSION_ACK",
                                            "PRE_UPDATER_UPDATE",
                                            "PATCH_COMMAND_WAIT",
                                            "PATCH_UPDATER_VERSION_ACK",
                                            "PATCH_DOWNLOAD_FILELIST",
                                            "PATCH_FILE_DOWNLOAD",
                                            "PATCH_FILEUNZIP",
                                            "PATCH_FILELIST_CHANGE",
                                            "PATCH_END",
                                            "FILE_CHECK",
                                            "FILE_CHECK_FOR_GAMESTART",
                                            "GAME_START",
                                            "FAIL",
                                            "ERROR"};
    log << "[UPDATER_STATE_" << names[static_cast<unsigned>(state)] << "] -> [UPDATER_STATE_"
        << names[static_cast<unsigned>(next)] << "]" << std::endl;
    state = next;
}
void Engine::reportFailure(ErrorCode code)
{
    if (config.host.empty())
        return;
    Bytes packet(8);
    const auto reportState = State::Error;
    std::memcpy(packet.data(), &reportState, 4);
    std::memcpy(packet.data() + 4, &code, 4);
    auto endpoint = config;
    endpoint.timeout = std::min(config.timeout, 3);
    try
    {
        queryVersion(endpoint, packet);
    }
    catch (...)
    { /* The local error remains available when the version server is offline. */
    }
}
Release Engine::queryVersion(const ClientConfig &endpoint, const Bytes &report)
{
    if (!versionSession)
        versionSession.emplace();
    return versionSession->query(endpoint, nativeVersion(modulePath()), [this] { checkpoint(); }, report);
}
Engine::~Engine()
{
    close();
    if (worker.joinable())
        worker.join();
}
void Engine::start()
{
    worker = std::thread(&Engine::run, this);
}
EngineView Engine::poll()
{
    std::lock_guard guard(mutex);
    auto result = view;
    view.error.clear();
    return result;
}
std::wstring Engine::messageText(const wchar_t *key, const wchar_t *fallback)
{
    std::lock_guard guard(mutex);
    return localizedText(messages, key, fallback);
}
void Engine::close()
{
    std::lock_guard guard(mutex);
    stop = true;
    change.notify_all();
}
void Engine::checkpoint()
{
    std::unique_lock lock(mutex);
    checkpoint(lock);
}
void Engine::checkpoint(std::unique_lock<std::mutex> &lock)
{
    change.wait(lock, [&] { return !paused || stop; });
    if (stop)
        throw Cancelled{};
}
void Engine::click(EngineCommand cmd)
{
    std::lock_guard guard(mutex);
    if (stop || pending)
        return;
    if (running)
    {
        if (view.primaryEnabled && cmd == view.primary && (cmd == EngineCommand::Pause || paused))
        {
            paused = cmd == EngineCommand::Pause;
            view.primary = paused ? EngineCommand::Update : EngineCommand::Pause;
            if (paused)
                view.status = localizedText(messages, L"STATE_PATCH_PAUSED", L"Atualização pausada.");
            else
                view.status = localizedText(messages, L"STATE_PATCH_RESUME", L"Retomando a atualização.");
            change.notify_all();
        }
        return;
    }
    if (!(((cmd == EngineCommand::Check || cmd == EngineCommand::CheckForStart) && view.check) ||
          (cmd == view.primary && view.primaryEnabled)))
        return;
    action = cmd;
    pending = true;
    view.check = view.primaryEnabled = false;
    change.notify_all();
}
void Engine::allow(EngineCommand primary)
{
    const bool canStart = primary == EngineCommand::Start;
    transition(canStart ? State::Ready : State::WaitUpdate);
    std::lock_guard guard(mutex);
    view.ready = true;
    view.primary = primary;
    view.primaryEnabled = true;
    view.check = canStart;
    view.status = canStart ? localizedText(messages, L"STATE_SIMPLE_PATCH_END", L"Você pode iniciar o jogo.")
                           : localizedText(messages, L"STATE_SIMPLE_UPDATE_WAIT", L"Clique em Update para atualizar o jogo.");
    view.file.clear();
    view.fileProgress = view.totalProgress = canStart ? ProgressRange{1, 1} : ProgressRange{};
    running = paused = false;
}
void Engine::display(const wchar_t *key, const wchar_t *fallback, const fs::path &file, ProgressRange overall)
{
    std::unique_lock guard(mutex);
    checkpoint(guard);
    view.status = localizedText(messages, key, fallback);
    view.file = file.wstring();
    view.totalProgress = overall;
    view.fileProgress = {};
}
Progress Engine::progress()
{
    return [this](uint64_t done, uint64_t total) {
        std::unique_lock guard(mutex);
        checkpoint(guard);
        view.fileProgress = {done, total};
    };
}
void Engine::run()
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto logPath = appDir / L"PBLauncher.log";
    std::error_code ec;
    auto bytes = fs::file_size(logPath, ec);
    log.open(logPath, !ec && (bytes >> 20) > 1 ? std::ios::trunc : std::ios::app);
    auto logTime = [this](const char *event) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        log << "PBLauncher " << event << " - " << now.wYear << '-' << now.wMonth << '-' << now.wDay << ' '
            << now.wHour << '-' << now.wMinute << '-' << now.wSecond << std::endl;
    };
    logTime("Start");
    log << "## PBLauncher Ver " << nativeVersion(modulePath()) << std::endl;
    transition(State::Start);
    while (true)
    {
        EngineCommand cmd;
        {
            std::unique_lock lock(mutex);
            if (!change.wait_for(lock, std::chrono::seconds(15), [&] { return stop || pending; }))
            {
                lock.unlock();
                try
                {
                    queryVersion(config);
                }
                catch (const Cancelled &)
                {
                    break;
                }
                catch (const std::exception &e)
                {
                    log << "TCP: " << e.what() << std::endl;
                }
                continue;
            }
            if (stop)
                break;
            cmd = action;
            pending = false;
            running = true;
        }
        try
        {
            if (cmd == EngineCommand::Connect)
                connect();
            else if (cmd == EngineCommand::Update)
                update();
            else if (cmd == EngineCommand::Check)
                checkFiles(false);
            else if (cmd == EngineCommand::Start || cmd == EngineCommand::CheckForStart)
            {
                if (cmd == EngineCommand::CheckForStart)
                    checkFiles(true);
                startGame();
                break;
            }
        }
        catch (const Cancelled &)
        {
            break;
        }
        catch (const std::exception &e)
        {
            const auto *specific = dynamic_cast<const LauncherError *>(&e);
            ErrorCode code = ErrorCode::FailedToLauncherUpdate;
            if (specific)
                code = specific->code;
            const bool recoverable = (static_cast<uint32_t>(code) >> 24) == 0x81;
            const auto failed = state;
            transition(recoverable ? State::Fail : State::Error);
            log << "state=" << static_cast<unsigned>(failed) << " code=0x" << std::hex
                << static_cast<uint32_t>(code) << std::dec << " " << e.what() << std::endl;
            std::wstring detail;
            try
            {
                detail = wide(e.what());
            }
            catch (const std::exception &)
            {
                detail = L"Falha ao acessar os arquivos do launcher.";
            }
            const auto *key = messageKey(code);
            if (key && messages.count(key))
                detail = messages.at(key) + L"\n\n" + detail;
            wchar_t errorNumber[16];
            swprintf_s(errorNumber, L" [0x%08X]", static_cast<unsigned>(code));
            detail += errorNumber;
            if (recoverable && cmd != EngineCommand::Connect)
            {
                if (local.version && local.version == remote.stamp)
                    allow(EngineCommand::Start);
                else
                    allow(EngineCommand::Update);
            }
            {
                std::lock_guard guard(mutex);
                if (!stop)
                    view.error = std::move(detail);
            }
            if (!recoverable)
            {
                reportFailure(code);
                transition(State::Unknown);
            }
            if (!recoverable || cmd == EngineCommand::Connect)
                break;
        }
    }
    versionSession.reset();
    logTime("End");
    if (SUCCEEDED(apartment))
        CoUninitialize();
    ended = true;
}
void Engine::connect()
{
    transition(State::Connect);
    if (!fs::is_directory(root))
        throw std::runtime_error("Diretório do jogo inexistente.");
    config = loadConfig(appDir);
    ntfs = isNtfs(root);
    reloadTexts();
    set = {};
    set.version = 0;
    auto localSet = insideRoot(appDir, L"Launcher.svl");
    if (fs::is_regular_file(localSet))
    {
        try
        {
            set = loadSet(localSet);
        }
        catch (const std::runtime_error &e)
        {
            log << "Launcher.svl: " << e.what() << std::endl;
        }
    }
    transition(State::PreVersion);
    remote = queryVersion(config);
    if (updateLauncher())
    {
        close();
        return;
    }
    loadLocal();
    if (local.version == remote.stamp)
        allow(EngineCommand::Start);
    else
        allow(EngineCommand::Update);
}
void Engine::loadLocal(bool allowSessionCopy)
{
    auto path = insideRoot(root, L"UserFileList.dat");
    std::error_code error;
    if (fs::is_regular_file(path, error))
    {
        try
        {
            local = Catalog::load(path);
            return;
        }
        catch (const std::runtime_error &e)
        {
            log << "UserFileList.dat: " << e.what() << std::endl;
        }
    }
    log << "failed user file list open" << std::endl;
    if (allowSessionCopy && local.version && local.version == remote.stamp && !local.files.empty())
        log << "Check: using the previously loaded UserFileList.dat for this version" << std::endl;
    else
        local = {};
}
void Engine::startGame()
{
    transition(State::StartGame);
    display(L"STATE_GAME_START", L"Iniciando o jogo.");
    // The original Start button queues state 14. Its version exchange must finish before ShellExecute.
    remote = queryVersion(config);
    checkpoint();
    const auto exe = insideRoot(appDir, config.gameExe);
    if (reserved(config.gameExe) || !fs::is_regular_file(exe))
        throw LauncherError(ErrorCode::NotFoundGameExe, "Executável do jogo indisponível: " + utf8(exe.wstring()));
    if (!gameAdministrator())
        throw LauncherError(ErrorCode::UserIsNotAdmin,
                            "A conta precisa pertencer ao grupo Administradores para iniciar o jogo.");
    try
    {
        startProcess(exe, L"1", appDir, L"runas");
    }
    catch (const std::runtime_error &e)
    {
        throw LauncherError(ErrorCode::CreateGameFail, e.what());
    }
    transition(State::Ready);
}
void Engine::checkFiles(bool beforeStart)
{
    transition(beforeStart ? State::CheckForStart : State::Check);
    loadLocal(!beforeStart);
    if (!local.version)
    {
        if (beforeStart)
            throw LauncherError(ErrorCode::NeededFilecheck, "Catálogo local indisponível.");
        allow(EngineCommand::Update);
        return;
    }
    std::vector<const FileEntry *> entries;
    entries.reserve(local.files.size());
    for (const auto &pair : local.files)
        entries.push_back(&pair.second);
    std::sort(entries.begin(), entries.end(), catalogOrder);
    struct Difference
    {
        const FileEntry *entry;
        unsigned code;
    };
    std::vector<Difference> differences;
    display(L"STATE_FILE_CHECK", L"Verificando a integridade dos arquivos.", {}, {0, entries.size()});
    const auto report = progress();
    for (const auto *entry : entries)
    {
        {
            std::unique_lock guard(mutex);
            checkpoint(guard);
            view.file = entry->path.filename().wstring();
        }
        const auto flags = beforeStart ? entry->startFlags : manualCheckFlags(*entry);
        if (flags && !reserved(entry->path))
        {
            const auto difference = fileDifference(insideRoot(root, entry->path), *entry, flags, ntfs, report);
            if (difference)
                differences.push_back({entry, difference});
        }
        {
            std::lock_guard guard(mutex);
            ++view.totalProgress.position;
        }
        // The original worker processes one catalog entry per tick (0x4175C0).
        Sleep(1);
    }
    if (!differences.empty())
    {
        log << "file check failed for " << (beforeStart ? "game start" : "check command") << " ["
            << differences.size() << "]\n>> detail begin\n";
        entries.clear();
        for (const auto &diff : differences)
        {
            entries.push_back(diff.entry);
            log << "[" << entries.size() << "/" << differences.size() << "] " << diff.entry->catalogKey
                << " / error : " << diff.code << '\n';
        }
        log << ">> detail end" << std::endl;
        if (beforeStart)
            throw LauncherError(ErrorCode::NeededFilecheck, "O jogo precisa de verificação.");
        Stage stage(root, L"_DownloadPatchFiles");
        applyFiles(entries, stage);
    }
    if (!beforeStart)
        allow(EngineCommand::Start);
}
void Engine::download(const fs::path &url, const fs::path &member, const fs::path &output,
                      const fs::path &archive, uint64_t limit)
{
    auto report = progress();
    fs::remove(archive);
    for (int attempt = 0;; ++attempt)
    {
        checkpoint();
        bool fetched = false;
        try
        {
            fetchZip(remote.base, url, archive, limit + limit / 8 + 1048576, report);
            fetched = true;
            {
                std::lock_guard guard(mutex);
                view.status = localizedText(messages, L"STATE_SIMPLE_FILE_UNZIP", L"Extraindo arquivos.");
            }
            extractZip(archive, member, output, limit, report);
            return;
        }
        catch (const LauncherError &)
        {
            throw;
        }
        catch (const std::runtime_error &err)
        {
            if (fetched)
                fs::remove(archive);
            if (attempt >= config.retries)
            {
                if (fetched)
                    throw LauncherError(ErrorCode::UzipError, err.what());
                throw;
            }
            std::lock_guard guard(mutex);
            view.status = localizedText(messages, L"STATE_RETRY_DOWNLOAD", L"Tentando baixar o arquivo novamente.");
        }
    }
}
void Engine::update()
{
    transition(State::PatchVersion);
    display(L"STATE_SIMPLE_VERSION_ACK", L"Consultando a versão do servidor.");
    remote = queryVersion(config);
    if (remote.launcher != set.version)
        throw LauncherError(ErrorCode::LauncherVerDiff,
                            "A versão do launcher mudou. Abra o launcher novamente.");
    loadLocal();
    Stage stage(root, L"_DownloadPatchFiles");
    const auto archive = stage.path / L"catalog.zip", manifest = stage.path / L"FileList.dat";
    auto filename = fs::path(wide(stampText(remote.stamp)) + L"_FileList.dat");
    display(L"STATE_DOWNLOAD_FILELIST", L"Baixando lista de arquivos.", filename);
    transition(State::DownloadList);
    try
    {
        download(filename.wstring() + L".zip", filename, manifest, archive, 64ULL << 20);
    }
    catch (const LauncherError &)
    {
        throw;
    }
    catch (const std::runtime_error &e)
    {
        throw LauncherError(ErrorCode::FilelistDownFailed, e.what());
    }
    Catalog target;
    try
    {
        target = Catalog::load(manifest);
        if (target.files.empty())
            throw std::runtime_error("Catálogo de atualização vazio.");
    }
    catch (const std::runtime_error &e)
    {
        throw LauncherError(ErrorCode::LatestfilelistOpenFailed, e.what());
    }
    if (target.version != remote.stamp)
        throw LauncherError(ErrorCode::FilelistCompareFailed, "Versão divergente no catálogo recebido.");
    std::vector<const FileEntry *> changed, added;
    for (const auto &[path, entry] : target.files)
    {
        checkpoint();
        if (reserved(path))
            continue;
        auto old = local.files.find(path);
        if (old == local.files.end())
            added.push_back(&entry);
        else if (old->second.catalogKey != entry.catalogKey || old->second.size != entry.size ||
                 old->second.md5 != entry.md5)
            changed.push_back(&entry);
    }
    std::sort(changed.begin(), changed.end(), catalogOrder);
    std::sort(added.begin(), added.end(), catalogOrder);
    changed.insert(changed.end(), added.begin(), added.end());
    applyFiles(changed, stage);
    for (const auto &[path, entry] : local.files)
    {
        checkpoint();
        if (!protectedOldFile(path) && !target.files.count(path))
        {
            auto obsolete = insideRoot(root, path);
            std::error_code ec;
            if (fs::is_regular_file(obsolete, ec))
                fs::remove(obsolete, ec);
            if (ec)
                log << "Arquivo obsoleto: " << utf8(path.wstring()) << " (" << ec.value() << ")\n";
        }
    }
    display(L"STATE_APPLY_FILELIST", L"Atualizando lista de arquivos.");
    try
    {
        replaceFile(manifest, insideRoot(root, L"UserFileList.dat"));
    }
    catch (const std::runtime_error &e)
    {
        throw LauncherError(ErrorCode::UserfilelistChangeFailed, e.what());
    }
    local = std::move(target);
    allow(EngineCommand::Start);
}
void Engine::setDownloadControls(bool enabled)
{
    std::unique_lock guard(mutex);
    checkpoint(guard);
    view.primary = EngineCommand::Pause;
    view.primaryEnabled = enabled;
}
void Engine::applyFiles(const std::vector<const FileEntry *> &files, Stage &stage)
{
    struct Work
    {
        const FileEntry *entry;
        fs::path archive, payload;
        bool extracted = false;
    };
    std::vector<Work> remaining;
    for (const auto *entry : files)
    {
        auto prefix = std::to_wstring(remaining.size());
        remaining.push_back({entry, stage.path / (prefix + L".zip"), stage.path / (prefix + L".new")});
    }
    for (int pass = 0;; ++pass)
    {
        uint64_t total = 0, completed = 0;
        for (const auto &item : remaining)
            total += item.entry->size;
        if ((fs::space(stage.path).available >> 20) < 2 * (total >> 20))
            throw LauncherError(ErrorCode::DiskFreeLack, "Espaço insuficiente para os arquivos do patch.");
        if (remaining.empty())
        {
            transition(State::ApplyList);
            return;
        }
        transition(State::DownloadFile);
        setDownloadControls(true);
        int downloadRetries = 0;
        for (auto &item : remaining)
        {
            auto &e = *item.entry;
            auto url = fs::path(wide(stampText(e.version))) / (e.path.wstring() + L".zip");
            display(L"STATE_SIMPLE_PATCHDOWN", L"Baixando arquivos.", e.path, {completed, total});
            auto notify = [&](uint64_t n, uint64_t length) {
                checkpoint();
                const auto bytes = length ? uint64_t(double(e.size) * std::min(n, length) / length) : 0;
                std::lock_guard lock(mutex);
                view.fileProgress = {n, length};
                view.totalProgress = {completed + bytes, total};
            };
            for (;;)
            {
                checkpoint();
                try
                {
                    fetchZip(remote.base, url, item.archive, uint64_t(e.size) + e.size / 8 + 1048576, notify);
                    break;
                }
                catch (const LauncherError &)
                {
                    throw;
                }
                catch (const std::runtime_error &err)
                {
                    // The original tests the batch counter before incrementing it.
                    if (downloadRetries > config.retries)
                        throw LauncherError(ErrorCode::PatchfileDownloadFailed, err.what());
                    ++downloadRetries;
                    log << "Retry download [" << e.catalogKey << "][" << downloadRetries << "]\n";
                }
            }
            completed += e.size;
        }
        setDownloadControls(false);
        transition(State::Unzip);
        size_t index = 0;
        for (auto &item : remaining)
        {
            auto &e = *item.entry;
            display(L"STATE_SIMPLE_FILE_UNZIP", L"Extraindo arquivos.", e.path, {index++, remaining.size()});
            try
            {
                extractZip(item.archive, e.path.filename(), item.payload, e.size, progress());
                item.extracted = true;
            }
            catch (const LauncherError &)
            {
                throw;
            }
            catch (const std::runtime_error &err)
            {
                log << err.what() << std::endl;
                item.extracted = false;
            }
            fs::remove(item.archive);
        }
        transition(State::ApplyList);
        std::vector<Work> retry;
        index = 0;
        for (auto &item : remaining)
        {
            auto &e = *item.entry;
            display(L"STATE_SIMPLE_PATCH_APPLY", L"Aplicando arquivos.", e.path, {index++, remaining.size()});
            if (!item.extracted || fileDifference(item.payload, e, 7, ntfs, progress()) != 0)
            {
                fs::remove(item.payload);
                retry.push_back(item);
                continue;
            }
            const auto dest = insideRoot(root, e.path);
            checkpoint();
            try
            {
                fs::create_directories(dest.parent_path());
                replaceFile(item.payload, dest);
                setFileDates(dest, e, ntfs);
            }
            catch (const std::runtime_error &err)
            {
                log << "Failed to apply " << e.catalogKey << ": " << err.what() << '\n';
                retry.push_back(item);
            }
        }
        if (retry.empty())
            return;
        if (pass >= config.retries)
        {
            std::string names;
            for (const auto &item : retry)
            {
                if (!names.empty())
                    names += ", ";
                names += utf8(item.entry->path.wstring());
            }
            throw LauncherError(ErrorCode::PatchfileMayBeCorrupted, "Falha de integridade: " + names);
        }
        remaining = std::move(retry);
    }
}
bool Engine::downloadLauncherSet(Stage &stage, const LauncherSet &target)
{
    auto archive = stage.path / L"download.zip";
    auto get = [&](std::wstring stem, const wchar_t *extension, const fs::path &out, uint64_t limit) {
        try
        {
            download(fs::path(L"_LauncherPatchFiles") / (stem + L".zip"), stem + extension, out, archive,
                     limit);
        }
        catch (const LauncherError &)
        {
            throw;
        }
        catch (const std::runtime_error &e)
        {
            throw LauncherError(ErrorCode::LauncherDownloadFailed, e.what());
        }
    };
    for (auto &c : components)
    {
        uint32_t v = target.components.at(c.name);
        if (installedComponent(appDir, c, target))
            continue;
        get(c.prefix + std::wstring(L"_") + std::to_wstring(v), c.extension, stage.path / c.name, 4 << 20);
        if (xmlVersion(stage.path / c.name, c.tag, c.rotated) != v)
            throw std::runtime_error("Versão de componente divergente.");
    }
    const auto version = target.components.at(L"Launcher");
    const bool swap = version != nativeVersion(modulePath());
    if (swap)
    {
        get(L"PBLauncher_" + std::to_wstring(version), L".exe", stage.path / L"next.exe", 128ULL << 20);
        if (!version || nativeVersion(stage.path / L"next.exe") != version)
            throw std::runtime_error("O pacote não contém a versão esperada de PBLauncher.exe.");
        if (!fs::is_regular_file(appDir / resourceCheckExecutable))
            throw LauncherError(ErrorCode::LauncherModifierRunFailed,
                                "ResourceCheck.exe ausente para a autoatualização.");
    }
    for (auto &c : components)
        if (fs::exists(stage.path / c.name))
            replaceFile(stage.path / c.name, insideRoot(appDir, c.name));
    if (swap)
        replaceFile(stage.path / L"next.exe", insideRoot(appDir, stagedLauncher));
    return swap;
}
bool Engine::updateLauncher()
{
    if (set.version && remote.launcher == set.version &&
        set.components.at(L"Launcher") == nativeVersion(modulePath()) &&
        std::all_of(std::begin(components), std::end(components),
                    [&](const Component &c) { return installedComponent(appDir, c, set); }))
        return false;
    display(L"STATE_LAUNCHER_UPDATE", L"Atualizando o launcher.");
    transition(State::LauncherUpdate);
    Stage stage(appDir, L"_LauncherPatchFiles");
    const auto stem = L"Launcher_" + std::to_wstring(remote.launcher);
    LauncherSet target = set;
    if (!set.version || remote.launcher != set.version)
    {
        download(fs::path(L"_LauncherPatchFiles") / (stem + L".zip"), stem + L".svl",
                 stage.path / L"Launcher.svl", stage.path / L"download.zip", 4 << 20);
        target = loadSet(stage.path / L"Launcher.svl");
        if (target.version != remote.launcher)
            throw LauncherError(ErrorCode::FailedToLauncherUpdate, "Versão de Launcher.svl divergente.");
    }
    bool swap = downloadLauncherSet(stage, target);
    if (fs::exists(stage.path / L"Launcher.svl"))
        replaceFile(stage.path / L"Launcher.svl", insideRoot(appDir, L"Launcher.svl"));
    set = std::move(target);
    if (swap)
    {
        try
        {
            startProcess(appDir / resourceCheckExecutable,
                         std::to_wstring(GetCurrentProcessId()) + L" " +
                             std::to_wstring(set.components.at(L"Launcher")) + L" \"" +
                             modulePath().filename().wstring() + L"\"",
                         appDir, L"open");
        }
        catch (const std::runtime_error &e)
        {
            throw LauncherError(ErrorCode::LauncherModifierRunFailed, e.what());
        }
    }
    else
    {
        config = loadConfig(appDir);
        reloadTexts();
    }
    return swap;
}
void Engine::reloadTexts()
{
    auto load = [this](const wchar_t *file, const wchar_t *element) {
        auto path = insideRoot(appDir, file);
        try
        {
            return loadTexts(path, element);
        }
        catch (const std::runtime_error &e)
        {
            log << utf8(file) << ": " << e.what() << std::endl;
            return std::map<std::wstring, std::wstring>{};
        }
    };
    auto updatedMessages = load(L"Messages.xml", L"message");
    auto ui = load(L"UI.xml", L"ui");
    std::lock_guard guard(mutex);
    messages = std::move(updatedMessages);
    view.hideNews = config.hideNews;
    view.newsUrl = config.newsUrl;
    if (ui.count(L"APP_TITLE"))
        view.title = ui.at(L"APP_TITLE");
    view.fileLabel = localizedText(messages, L"MAIN_FILE", localizedText(ui, L"MAIN_FILE", L"Arquivo"));
    view.totalLabel = localizedText(messages, L"MAIN_TOTAL", localizedText(ui, L"MAIN_TOTAL", L"Total"));
    view.pauseLabel = localizedText(messages, L"BUTTON_PAUSE", L"Pausar");
}
