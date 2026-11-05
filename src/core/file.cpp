#include "core/file.h"

#ifdef _WIN32
#include <Windows.h>
#include <sddl.h>

#include <memory>
#include <vector>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
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

// Writes text whole and flushes it to disk.
auto writeAndFlush(const HANDLE file, const std::string_view text) -> bool
{
    DWORD written{ 0 };
    return ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size() && ::FlushFileBuffers(file);
}

} // namespace
#else
namespace
{

// Writes text whole and flushes it to disk.
auto writeAndFlush(const int fd, const std::string_view text) -> bool
{
    for (size_t at = 0; at < text.size();)
    {
        const auto n = ::write(fd, text.data() + at, text.size() - at);
        if (n <= 0)
        {
            return false;
        }
        at += static_cast<size_t>(n);
    }
    return ::fsync(fd) == 0;
}

} // namespace
#endif

// Not std::ios::noreplace: Wine, which runs the tests, has no fopen "x" mode.
auto createNew(const std::filesystem::path& path, const bool secret, const std::string_view text) -> bool
{
#ifdef _WIN32
    PSECURITY_DESCRIPTOR sd{ nullptr };
    if (secret)
    {
        // Full access for this user alone, nothing inherited from the folder, which other users of the PC may read.
        const auto sid = userSid();
        if (sid.empty() || !::ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;;FA;;;" + sid + L")").c_str(), SDDL_REVISION_1, &sd, nullptr))
        {
            return false;
        }
    }
    const std::unique_ptr<void, LocalFreer> owned(sd);
    SECURITY_ATTRIBUTES                     sa{ .nLength = sizeof(sa), .lpSecurityDescriptor = sd, .bInheritHandle = FALSE };
    const HANDLE                            file{ ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) };
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const bool ok{ text.empty() || writeAndFlush(file, text) };
    return ::CloseHandle(file) && ok;
#else
    const int fd{ ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, secret ? 0600 : 0644) };
    if (fd < 0)
    {
        return false;
    }
    const bool ok{ text.empty() || writeAndFlush(fd, text) };
    return ::close(fd) == 0 && ok;
#endif
}

auto moveNoReplace(const std::filesystem::path& from, const std::filesystem::path& to) -> bool
{
#ifdef _WIN32
    return ::MoveFileExW(from.c_str(), to.c_str(), 0) != 0;
#elifdef __linux__
    return ::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0;
#else
    return ::link(from.c_str(), to.c_str()) == 0 && ::unlink(from.c_str()) == 0;
#endif
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
    const bool flushed{ writeAndFlush(file, text) && !(failSave && failSave(SaveStep::Flush)) };
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
    const bool ok{ writeAndFlush(fd, text) && !(failSave && failSave(SaveStep::Flush)) };
    if (::close(fd) != 0 || !ok)
    {
        return unwritten(true);
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0)
    {
        auto error = std::format("cannot save {}: {}", path.filename().string(), std::strerror(errno));
        ::unlink(tmp.c_str());
        return error;
    }
    const auto dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const int  dfd{ ::open(dir.c_str(), O_RDONLY | O_DIRECTORY) };
    const bool synced{ dfd >= 0 && ::fsync(dfd) == 0 && !(failSave && failSave(SaveStep::FolderFlush)) };
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
