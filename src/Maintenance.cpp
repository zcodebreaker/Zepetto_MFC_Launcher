#include "Catalog.h"
#include "NativeVersion.h"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        int count = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args || (count != 3 && count != 4))
            throw std::runtime_error("Abra PBLauncher.exe para iniciar a atualização.");
        const auto pid = std::stoul(args[1]), version = std::stoul(args[2]);
        auto target = pb::relativeFile(count == 4 ? args[3] : pb::launcherExecutable);
        LocalFree(args);
        if (target.has_parent_path() || _wcsicmp(target.extension().c_str(), L".exe"))
            throw std::runtime_error("Nome do launcher inválido.");
        auto dir = pb::appFolder();
        auto exe = pb::insideRoot(dir, target);
        auto next = pb::insideRoot(dir, pb::stagedLauncher);
        auto previous = pb::insideRoot(dir, pb::previousLauncher);
        if (!version || !pb::nativeVersion(exe) || pb::nativeVersion(next) != version)
            throw std::runtime_error("Pacote do launcher incompatível.");
        HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (parent)
        {
            wchar_t actual[32768];
            DWORD len = ARRAYSIZE(actual);
            bool expected =
                QueryFullProcessImageNameW(parent, 0, actual, &len) && !_wcsicmp(actual, exe.c_str());
            DWORD wait = expected ? WaitForSingleObject(parent, 60000) : WAIT_FAILED;
            CloseHandle(parent);
            if (wait != WAIT_OBJECT_0)
                throw std::runtime_error("Falha ao aguardar o fechamento do launcher.");
        }
        if (!ReplaceFileW(exe.c_str(), next.c_str(), previous.c_str(), 0, nullptr, nullptr))
            throw std::runtime_error("Falha ao instalar a nova versão do launcher.");
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
        {
            pb::replaceFile(previous, exe);
            throw std::runtime_error("Falha ao iniciar a nova versão. A versão anterior foi restaurada.");
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::error_code ec;
        std::filesystem::remove(previous, ec);
        return 0;
    }
    catch (const std::exception &e)
    {
        MessageBoxW(nullptr, pb::wide(e.what()).c_str(), L"PBLauncher", MB_OK);
        return 1;
    }
}
