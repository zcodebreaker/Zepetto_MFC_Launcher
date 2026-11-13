#pragma once
#include "Protocol.h"
#include <functional>
#include <map>

namespace pb
{
using Digest = std::array<unsigned char, 16>;
using Progress = std::function<void(uint64_t, uint64_t)>;
struct FileEntry
{
    fs::path path;
    std::string catalogKey;
    uint64_t version{};
    uint32_t size{};
    Digest md5{};
    FILETIME creation{}, modified{};
    uint32_t startFlags = 11, checkFlags = 15;
};
struct PathOrder
{
    bool operator()(const fs::path &a, const fs::path &b) const
    {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};
struct Catalog
{
    uint64_t version{};
    std::map<fs::path, FileEntry, PathOrder> files;
    static Catalog load(const fs::path &path);
};
Bytes readBytes(const fs::path &path, uint64_t limit);
void decodeRotation(Bytes &bytes, unsigned shift);
fs::path relativeFile(const std::wstring &text);
fs::path insideRoot(const fs::path &root, const fs::path &relative);
bool isNtfs(const fs::path &path);
unsigned fileDifference(const fs::path &path, const FileEntry &file, uint32_t flags, bool ntfs,
                        const Progress &progress);
void setFileDates(const fs::path &path, const FileEntry &file, bool ntfs);
void replaceFile(const fs::path &source, const fs::path &destination);
} // namespace pb
