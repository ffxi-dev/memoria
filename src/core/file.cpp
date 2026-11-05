#include "core/file.h"

#ifdef _WIN32
#include <Windows.h>
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

#include <cerrno>
#include <cstring>
#endif

#include <format>

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
    HANDLE token{ nullptr };
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return {};
    }
    DWORD size{ 0 };
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    wchar_t*               text{ nullptr };
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
auto createNew(const std::filesystem::path& path, const bool secret) -> bool
{
#ifdef _WIN32
    if (secret)
    {
        // Full access for this user alone, nothing inherited from the folder, which other users of the PC may read.
        const auto           sid = userSid();
        PSECURITY_DESCRIPTOR sd{ nullptr };
        if (sid.empty() || !::ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;;FA;;;" + sid + L")").c_str(), SDDL_REVISION_1, &sd, nullptr))
        {
            return false;
        }
        const std::unique_ptr<void, LocalFreer> owned(sd);
        SECURITY_ATTRIBUTES                     sa{ .nLength = sizeof(sa), .lpSecurityDescriptor = sd, .bInheritHandle = FALSE };
        const HANDLE                            file{ ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        ::CloseHandle(file);
        return true;
    }
    int fd{ -1 };
    if (_wsopen_s(&fd, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
    {
        return false;
    }
    _close(fd);
#else
    const int fd{ ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, secret ? 0600 : 0644) };
    if (fd < 0)
    {
        return false;
    }
    ::close(fd);
#endif
    return true;
}

auto replaceFile(const std::filesystem::path& path, const std::string_view text) -> std::optional<std::string>
{
    const auto tmp       = std::filesystem::path(path).concat(".tmp");
    const auto unwritten = [&](const bool created)
    {
        if (created)
        {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        }
        return std::format("cannot write {}", tmp.string());
    };
#ifdef _WIN32
    const HANDLE file{ ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
    if (file == INVALID_HANDLE_VALUE)
    {
        return unwritten(false);
    }
    DWORD      written{ 0 };
    const bool flushed{ ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size() && ::FlushFileBuffers(file) };
    if (!::CloseHandle(file) || !flushed)
    {
        return unwritten(true);
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const auto error = ::GetLastError();
        ::DeleteFileW(tmp.c_str());
        return std::format("cannot save {}: Windows error {}", path.filename().string(), error);
    }
#else
    const int fd{ ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644) };
    if (fd < 0)
    {
        return unwritten(false);
    }
    bool ok{ true };
    for (size_t at = 0; ok && at < text.size();)
    {
        const auto n = ::write(fd, text.data() + at, text.size() - at);
        ok           = n > 0;
        at += ok ? static_cast<size_t>(n) : 0;
    }
    ok = ok && ::fsync(fd) == 0;
    if (::close(fd) != 0 || !ok)
    {
        return unwritten(true);
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0)
    {
        const auto error = std::format("cannot save {}: {}", path.filename().string(), std::strerror(errno));
        ::unlink(tmp.c_str());
        return error;
    }
    const auto dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const int  dfd{ ::open(dir.c_str(), O_RDONLY | O_DIRECTORY) };
    const bool synced{ dfd >= 0 && ::fsync(dfd) == 0 };
    if (dfd >= 0)
    {
        ::close(dfd);
    }
    if (!synced)
    {
        return std::format("saved {}, but cannot flush its folder", path.filename().string());
    }
#endif
    return std::nullopt;
}

} // namespace memoria
