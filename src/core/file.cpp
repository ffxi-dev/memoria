#include "core/file.h"

#ifdef _WIN32
#include <Windows.h>
#include <aclapi.h>
#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#include <share.h>
#include <sys/stat.h>

#include <memory>
#include <vector>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace memoria
{

#ifdef _WIN32
namespace
{

struct LocalFreer
{
    auto operator()(void* p) const -> void
    {
        ::LocalFree(p);
    }
};

// The user running the game, as a string SID; empty if Windows will not say.
auto userSid() -> std::wstring
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return {};
    }
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    wchar_t*               text = nullptr;
    if (size > 0 && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
    {
        ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text);
    }
    ::CloseHandle(token);
    const std::unique_ptr<wchar_t, LocalFreer> owned(text);
    return text ? std::wstring(text) : std::wstring{};
}

} // namespace
#endif

// Not std::ios::noreplace: Wine, which runs the tests, has no fopen "x" mode.
auto createNew(const std::filesystem::path& path, bool secret) -> bool
{
#ifdef _WIN32
    if (secret)
    {
        // Full access for this user alone, nothing inherited from the folder, which other users of the PC may read.
        const auto           sid = userSid();
        PSECURITY_DESCRIPTOR sd  = nullptr;
        if (sid.empty() || !::ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;;FA;;;" + sid + L")").c_str(), SDDL_REVISION_1, &sd, nullptr))
        {
            return false;
        }
        const std::unique_ptr<void, LocalFreer> owned(sd);
        SECURITY_ATTRIBUTES                     sa{ .nLength = sizeof(sa), .lpSecurityDescriptor = sd, .bInheritHandle = FALSE };
        const HANDLE                            file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        ::CloseHandle(file);
        return true;
    }
    int fd = -1;
    if (_wsopen_s(&fd, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
    {
        return false;
    }
    _close(fd);
#else
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, secret ? 0600 : 0644);
    if (fd < 0)
    {
        return false;
    }
    ::close(fd);
#endif
    return true;
}

auto ownerOnly(const std::filesystem::path& path) -> bool
{
#ifdef _WIN32
    PACL                 dacl = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS)
    {
        return false;
    }
    const std::unique_ptr<void, LocalFreer> owned(sd);
    SECURITY_DESCRIPTOR_CONTROL             control  = 0;
    DWORD                                   revision = 0;
    if (dacl == nullptr || !::GetSecurityDescriptorControl(sd, &control, &revision) || (control & SE_DACL_PROTECTED) == 0)
    {
        return false;
    }
    const auto sid = userSid();
    for (DWORD i = 0; i < dacl->AceCount; ++i)
    {
        void* ace = nullptr;
        if (!::GetAce(dacl, i, &ace) || static_cast<ACE_HEADER*>(ace)->AceType != ACCESS_ALLOWED_ACE_TYPE)
        {
            return false;
        }
        wchar_t* text = nullptr;
        ::ConvertSidToStringSidW(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart, &text);
        const std::unique_ptr<wchar_t, LocalFreer> name(text);
        if (!text || sid != text)
        {
            return false;
        }
    }
    return true;
#else
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && (st.st_mode & 077) == 0;
#endif
}

} // namespace memoria
