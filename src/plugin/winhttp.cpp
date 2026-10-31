#include "plugin/winhttp.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
#include <ranges>
#include <string>

namespace memoria
{
namespace
{

struct CloseHandle
{
    auto operator()(HINTERNET h) const -> void
    {
        WinHttpCloseHandle(h);
    }
};
using Handle = std::unique_ptr<void, CloseHandle>;

// URLs and headers here are ASCII.
auto wide(std::string_view s) -> std::wstring
{
    return { s.begin(), s.end() };
}

auto narrow(std::wstring_view s) -> std::string
{
    return s | std::views::transform([](wchar_t c)
                                     {
                                         return static_cast<char>(c);
                                     }) |
           std::ranges::to<std::string>();
}

auto failed(const char* what) -> std::unexpected<std::string>
{
    return std::unexpected(std::format("{}: WinHTTP error {}", what, GetLastError()));
}

auto trim(std::string_view s) -> std::string_view
{
    const auto first = s.find_first_not_of(' ');
    return first == std::string_view::npos ? std::string_view{} : s.substr(first);
}

} // namespace

WinHttp::WinHttp(const std::string& agent)
: session_(WinHttpOpen(wide(agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0))
{
    if (session_)
    {
        // Mogsafe hashes and checks a whole capture before it answers a close.
        WinHttpSetTimeouts(session_, 10'000, 10'000, 120'000, 120'000);
    }
}

WinHttp::~WinHttp()
{
    if (session_)
    {
        WinHttpCloseHandle(session_);
    }
}

auto WinHttp::request(const std::string& method, const std::string& url, const HttpHeaders& headers, std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string>
{
    if (!session_)
    {
        return std::unexpected("WinHTTP did not start");
    }
    const auto     wurl = wide(url);
    URL_COMPONENTS parts{ .dwStructSize = sizeof(URL_COMPONENTS), .dwHostNameLength = DWORD(-1), .dwUrlPathLength = DWORD(-1), .dwExtraInfoLength = DWORD(-1) };
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts))
    {
        return failed("bad URL");
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    const Handle connection(WinHttpConnect(session_, host.c_str(), parts.nPort, 0));
    if (!connection)
    {
        return failed("connect");
    }
    const Handle req(WinHttpOpenRequest(connection.get(), wide(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!req)
    {
        return failed("open request");
    }
    std::wstring lines;
    for (const auto& [name, value] : headers)
    {
        lines += std::format(L"{}: {}\r\n", wide(name), wide(value));
    }
    auto*      data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<uint8_t*>(body.data());
    const auto size = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(req.get(), lines.c_str(), DWORD(-1), data, size, size, 0) || !WinHttpReceiveResponse(req.get(), nullptr))
    {
        return failed("send");
    }

    HttpResponse res;
    DWORD        status = 0;
    DWORD        len    = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    res.status = static_cast<int>(status);

    len = 0;
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &len, WINHTTP_NO_HEADER_INDEX);
    std::wstring raw(len / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &len, WINHTTP_NO_HEADER_INDEX))
    {
        const auto text = narrow(raw);
        for (const auto line : std::string_view(text) | std::views::split(std::string_view("\r\n")) | std::views::drop(1)) // the status line
        {
            const std::string_view l(line);
            if (const auto colon = l.find(':'); colon != std::string_view::npos)
            {
                auto name = std::string(l.substr(0, colon));
                std::ranges::transform(name, name.begin(), [](unsigned char c)
                                       {
                                           return static_cast<char>(std::tolower(c));
                                       });
                res.headers[name] = trim(l.substr(colon + 1));
            }
        }
    }

    for (DWORD available = 0; WinHttpQueryDataAvailable(req.get(), &available) && available > 0;)
    {
        std::string chunk(available, '\0');
        DWORD       read = 0;
        if (!WinHttpReadData(req.get(), chunk.data(), available, &read) || read == 0)
        {
            break;
        }
        res.body.append(chunk, 0, read);
    }
    return res;
}

} // namespace memoria
