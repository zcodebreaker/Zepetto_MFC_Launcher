#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pb
{
enum class ErrorCode : uint32_t
{
    DiskFreeLack = 0x81000002u,
    NeededFilecheck = 0x81000003u,
    CreateGameFail = 0x81000004u,
    ConnectFail2 = 0x81000005u,
    SocketsInitFailed = 0x82000002u,
    LatestfilelistOpenFailed = 0x82000005u,
    FilelistCompareFailed = 0x82000006u,
    UserfilelistChangeFailed = 0x8200000Au,
    LatestfilelistDeleteFailed = 0x8200000Bu,
    PatchfileDownloadFailed = 0x8200000Cu,
    FileDownloadReqFailed = 0x8200000Du,
    FilelistDownFailed = 0x8200000Eu,
    ReposConnectFail = 0x82000010u,
    LauncherVerDiff = 0x82000011u,
    LauncherDownloadFailed = 0x82000015u,
    LauncherModifierRunFailed = 0x82000016u,
    NotFoundGameExe = 0x82000017u,
    UzipError = 0x82000019u,
    ConvertDomainToIpFailed = 0x82000020u,
    NetworkInitFail = 0x82000023u,
    CannotGetModuleName = 0x82000024u,
    CannotGetFilesystemName = 0x82000025u,
    DownloadRestartFailed = 0x82000026u,
    PatchfileMayBeCorrupted = 0x82000027u,
    ServerAckTimeout = 0x82000028u,
    FailedToLauncherUpdate = 0x82000029u,
    UserIsNotAdmin = 0x82000030u,
};
inline constexpr const wchar_t *messageKeys[] = {
    L"SOCKETS_INIT_FAILED",
    L"UPDATER_INIT_FAILED",
    L"FILELIST_OPEN_FAILED",
    L"DUPLICATE_EXE",
    L"RUNTIME_LONG",
    L"DO_NOT_EXE",
    L"DISK_FREE_LACK",
    L"NEEDED_FILECHECK",
    L"CREATE_GAME_FAIL",
    L"CMDE_UPDATE_START",
    L"CMDE_UPDATE_PAUSE",
    L"CMDE_FILECHECK1_FAIL",
    L"QUESTION_EXIT",
    L"REPOS_CONNECT_FAIL",
    L"INIT_WAIT_MSG",
    L"STATE_FILE_CHECK",
    L"STATE_SIMPLE_VERSION_ACK",
    L"STATE_SIMPLE_UPDATE_WAIT",
    L"STATE_SIMPLE_PATCHDOWN",
    L"STATE_SIMPLE_FILE_UNZIP",
    L"STATE_SIMPLE_PATCH_APPLY",
    L"STATE_SIMPLE_PATCH_END",
    L"STATE_SIMPLE_ERROR",
    L"STATE_SIMPLE_PATCHDOWN_END",
    L"STATE_SIMPLE_VERSION_CHECK",
    L"CONNECT_FAIL2",
    L"NETWORK_INIT_FAIL",
    L"LAUNCHER_VER_DIFF",
    L"LATESTFILELIST_OPEN_FAILED",
    L"FILELIST_COMPARE_FAILED",
    L"USERFILELIST_CHANGE_FAILED",
    L"LATESTFILELIST_DELETE_FAILED",
    L"PATCHFILE_DOWNLOAD_FAILED",
    L"FILE_DOWNLOAD_REQ_FAILED",
    L"FILELIST_DOWN_FAILED",
    L"LAUNCHER_DOWNLOAD_FAILED",
    L"LAUNCHER_MODIFIER_RUN_FAILED",
    L"NOT_FOUND_GAME_EXE",
    L"UZIP_ERROR",
    L"CONVERT_DOMAIN_TO_IP_FAILED",
    L"CANNOT_GET_MODULE_NAME",
    L"CANNOT_GET_FILESYSTEM_NAME",
    L"DOWNLOAD_RESTART_FAILED",
    L"PATCHFILE_MAY_BE_CORRUPTED",
    L"SERVER_ACK_TIMEOUT",
    L"FAILED_TO_LAUNCHER_UPDATE",
    L"USER_IS_NOT_ADMIN",
};
inline const wchar_t *messageKey(ErrorCode code)
{
    switch (code)
    {
    case ErrorCode::DiskFreeLack:
        return messageKeys[6];
    case ErrorCode::NeededFilecheck:
        return messageKeys[7];
    case ErrorCode::CreateGameFail:
        return messageKeys[8];
    case ErrorCode::ConnectFail2:
        return messageKeys[25];
    case ErrorCode::SocketsInitFailed:
        return messageKeys[0];
    case ErrorCode::LatestfilelistOpenFailed:
        return messageKeys[28];
    case ErrorCode::FilelistCompareFailed:
        return messageKeys[29];
    case ErrorCode::UserfilelistChangeFailed:
        return messageKeys[30];
    case ErrorCode::LatestfilelistDeleteFailed:
        return messageKeys[31];
    case ErrorCode::PatchfileDownloadFailed:
        return messageKeys[32];
    case ErrorCode::FileDownloadReqFailed:
        return messageKeys[33];
    case ErrorCode::FilelistDownFailed:
        return messageKeys[34];
    case ErrorCode::ReposConnectFail:
        return messageKeys[13];
    case ErrorCode::LauncherVerDiff:
        return messageKeys[27];
    case ErrorCode::LauncherDownloadFailed:
        return messageKeys[35];
    case ErrorCode::LauncherModifierRunFailed:
        return messageKeys[36];
    case ErrorCode::NotFoundGameExe:
        return messageKeys[37];
    case ErrorCode::UzipError:
        return messageKeys[38];
    case ErrorCode::ConvertDomainToIpFailed:
        return messageKeys[39];
    case ErrorCode::NetworkInitFail:
        return messageKeys[26];
    case ErrorCode::CannotGetModuleName:
        return messageKeys[40];
    case ErrorCode::CannotGetFilesystemName:
        return messageKeys[41];
    case ErrorCode::DownloadRestartFailed:
        return messageKeys[42];
    case ErrorCode::PatchfileMayBeCorrupted:
        return messageKeys[43];
    case ErrorCode::ServerAckTimeout:
        return messageKeys[44];
    case ErrorCode::FailedToLauncherUpdate:
        return messageKeys[45];
    case ErrorCode::UserIsNotAdmin:
        return messageKeys[46];
    }
    return nullptr;
}
struct LauncherError : std::runtime_error
{
    ErrorCode code;
    LauncherError(ErrorCode value, const std::string &detail) : std::runtime_error(detail), code(value)
    {
    }
};
} // namespace pb
