#pragma once
#include <windows.h>
#include <filesystem>
#include <cstdint>
#include <cstring>
namespace pb
{
inline constexpr wchar_t launcherExecutable[] = L"PBLauncher.exe";
inline constexpr wchar_t resourceCheckExecutable[] = L"ResourceCheck.exe";
inline constexpr wchar_t stagedLauncher[] = L"PBLauncher.next.exe";
inline constexpr wchar_t previousLauncher[] = L"PBLauncher.previous.exe";

// Identifies builds of this independent client when installing a launcher package.
inline uint32_t nativeVersion(const std::filesystem::path &file)
{
    HMODULE image = LoadLibraryExW(file.c_str(), nullptr,
                                   LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!image)
        return 0;
    const auto resource = FindResourceW(image, MAKEINTRESOURCEW(350), RT_RCDATA);
    const auto data = resource ? LockResource(LoadResource(image, resource)) : nullptr;
    uint32_t version = 0;
    if (data && SizeofResource(image, resource) == 12 && !std::memcmp(data, "MFC-PB2!", 8))
        std::memcpy(&version, static_cast<const char *>(data) + 8, 4);
    FreeLibrary(image);
    return version;
}
} // namespace pb
