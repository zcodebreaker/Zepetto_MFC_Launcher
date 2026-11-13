#pragma once
#include "Catalog.h"

namespace pb
{
struct ClientConfig
{
    std::wstring host;
    int port = 0, retries = 3, timeout = 30;
    std::wstring newsUrl;
    bool hideNews = false;
    fs::path gameExe;
};
struct LauncherSet
{
    uint32_t version = 20171214;
    std::map<std::wstring, uint32_t> components;
};
ClientConfig loadConfig(const fs::path &app);
std::map<std::wstring, std::wstring> loadTexts(const fs::path &path, const wchar_t *root);
std::wstring localizedText(const std::map<std::wstring, std::wstring> &texts, const wchar_t *key,
                           const std::wstring &fallback);
LauncherSet loadSet(const fs::path &path);
uint32_t xmlVersion(const fs::path &path, const std::wstring &element, bool rotated);
class VersionSession
{
    Winsock winsock;
    Socket connection;
    std::wstring host;
    int port = 0;
  public:
    Release query(const ClientConfig &config, uint32_t version, const std::function<void()> &checkpoint,
                  const Bytes &report = {});
};
void fetchZip(const std::string &base, const fs::path &remote, const fs::path &archive, uint64_t limit,
              const Progress &progress);
void extractZip(const fs::path &archive, const fs::path &name, const fs::path &output, uint64_t limit,
                const Progress &progress);
class Stage
{
  public:
    fs::path path;
    Stage(const fs::path &root, const wchar_t *folder);
    ~Stage();
    Stage(const Stage &) = delete;
};
} // namespace pb
