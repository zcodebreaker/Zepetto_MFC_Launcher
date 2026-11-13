#pragma once
#include "PatchIO.h"
#include "LauncherError.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

enum class EngineCommand
{
    Connect,
    Start,
    Update,
    Pause,
    CheckForStart,
    Check
};
struct ProgressRange
{
    uint64_t position = 0, maximum = 0;
};
struct EngineView
{
    bool ready = false, hideNews = false, check = false, primaryEnabled = false;
    EngineCommand primary = EngineCommand::Update;
    ProgressRange fileProgress, totalProgress;
    std::wstring status, file, error, newsUrl;
    std::wstring title = L"PBLauncher", fileLabel, totalLabel, pauseLabel;
};
class Engine
{
  public:
    explicit Engine(pb::fs::path directory, std::map<std::wstring, std::wstring> initialMessages = {});
    ~Engine();
    void start();
    EngineView poll();
    std::wstring messageText(const wchar_t *key, const wchar_t *fallback);
    void click(EngineCommand command);
    void close();
    bool finished() const
    {
        return ended;
    }
    bool active() const
    {
        return worker.joinable() && !ended;
    }

  private:
    pb::fs::path root, appDir;
    pb::ClientConfig config;
    std::optional<pb::VersionSession> versionSession;
    pb::LauncherSet set;
    pb::Release remote{};
    pb::Catalog local;
    bool ntfs = true;
    std::map<std::wstring, std::wstring> messages;
    std::mutex mutex;
    std::condition_variable change;
    EngineView view;
    std::thread worker;
    std::atomic<bool> ended{false};
    bool stop = false, paused = false, pending = true, running = true;
    EngineCommand action = EngineCommand::Connect;
    enum class State : uint32_t
    {
        Unknown,
        Start,
        Connect,
        PreVersion,
        LauncherUpdate,
        WaitUpdate,
        PatchVersion,
        DownloadList,
        DownloadFile,
        Unzip,
        ApplyList,
        Ready,
        Check,
        CheckForStart,
        StartGame,
        Fail,
        Error
    };
    State state = State::Unknown;
    std::ofstream log;
    void transition(State next);
    void reportFailure(pb::ErrorCode code);
    pb::Release queryVersion(const pb::ClientConfig &endpoint, const pb::Bytes &report = {});
    void run();
    void checkpoint();
    void checkpoint(std::unique_lock<std::mutex> &lock);
    void allow(EngineCommand primary);
    void display(const wchar_t *key, const wchar_t *fallback, const pb::fs::path &file = {}, ProgressRange overall = {});
    pb::Progress progress();
    void connect();
    void update();
    void checkFiles(bool beforeStart);
    void startGame();
    void applyFiles(const std::vector<const pb::FileEntry *> &files, pb::Stage &stage);
    void setDownloadControls(bool enabled);
    void loadLocal(bool allowSessionCopy = false);
    bool updateLauncher();
    void reloadTexts();
    bool downloadLauncherSet(pb::Stage &stage, const pb::LauncherSet &target);
    void download(const pb::fs::path &remotePath, const pb::fs::path &member, const pb::fs::path &output,
                  const pb::fs::path &archive, uint64_t limit);
};
