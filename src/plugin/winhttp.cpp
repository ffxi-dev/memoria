#include "plugin/winhttp.h"

#include "core/units.h"

#include <Windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

namespace memoria::plugin
{
namespace
{

auto narrow(const std::wstring_view s) -> std::string
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

auto trim(const std::string_view s) -> std::string_view
{
    const auto first = s.find_first_not_of(' ');
    return first == std::string_view::npos ? std::string_view{} : s.substr(first);
}

auto stopped() -> std::unexpected<std::string>
{
    return std::unexpected<std::string>("stopped");
}

auto perform(const HINTERNET req, const bool head, const HttpHeaders& headers, const std::span<const uint8_t> body, const std::atomic<bool>& cancelled) -> std::expected<HttpResponse, std::string>
{
    std::wstring lines;
    for (const auto& [name, value] : headers)
    {
        lines += std::format(L"{}: {}\r\n", wide(name), wide(value));
    }
    if (!WinHttpSendRequest(req, lines.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(body.size()), 0))
    {
        return failed("send");
    }
    for (size_t at = 0; at < body.size();)
    {
        if (cancelled)
        {
            return stopped();
        }
        DWORD      written{ 0 };
        const auto n = static_cast<DWORD>((std::min)(body.size() - at, WRITE_SIZE));
        if (!WinHttpWriteData(req, body.data() + at, n, &written) || written == 0)
        {
            return failed("send");
        }
        at += written;
    }
    if (cancelled)
    {
        return stopped();
    }
    if (!WinHttpReceiveResponse(req, nullptr))
    {
        return failed("receive");
    }

    HttpResponse res;
    DWORD        status{ 0 };
    DWORD        len{ sizeof(status) };
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX))
    {
        return failed("receive");
    }
    res.status = static_cast<int>(status);

    len = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &len, WINHTTP_NO_HEADER_INDEX);
    std::wstring raw(len / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &len, WINHTTP_NO_HEADER_INDEX))
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

    for (DWORD available = 0;;)
    {
        if (cancelled)
        {
            return stopped();
        }
        // A body cut short is a transport failure: only a whole answer is a response.
        if (!WinHttpQueryDataAvailable(req, &available))
        {
            return failed("receive");
        }
        if (available == 0)
        {
            break;
        }
        std::string chunk(available, '\0');
        DWORD       read{ 0 };
        if (!WinHttpReadData(req, chunk.data(), available, &read))
        {
            return failed("receive");
        }
        if (read == 0)
        {
            break;
        }
        res.body.append(chunk, 0, read);
    }
    // WinHTTP ends a body cut off by a closed connection as if it were whole. A HEAD answer's length is the body it omits.
    if (const auto length = parseU64(header(res, "content-length")); !head && length && *length != res.body.size())
    {
        return std::unexpected(std::format("receive: {} of {} bytes", res.body.size(), *length));
    }
    return res;
}

} // namespace

auto CloseInternetHandle::operator()(void* handle) const -> void
{
    WinHttpCloseHandle(handle);
}

// URLs and headers here are ASCII.
auto wide(const std::string_view ascii) -> std::wstring
{
    return { ascii.begin(), ascii.end() };
}

auto browse(const std::string& url) -> void
{
    if (!url.starts_with("https://"))
    {
        return;
    }
    const auto com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ::ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (SUCCEEDED(com))
    {
        ::CoUninitialize();
    }
}

auto openSession() -> InternetHandle
{
    return InternetHandle(WinHttpOpen(L"Memoria/" MEMORIA_VERSION_TEXT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
}

auto openRequest(void* session, const std::string_view method, const std::string& url) -> std::expected<OpenRequest, std::string>
{
    const auto     wurl = wide(url);
    URL_COMPONENTS parts{ .dwStructSize = sizeof(URL_COMPONENTS), .dwHostNameLength = static_cast<DWORD>(-1), .dwUrlPathLength = static_cast<DWORD>(-1), .dwExtraInfoLength = static_cast<DWORD>(-1) };
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts))
    {
        return failed("bad URL");
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path{ std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength) };

    OpenRequest open{ .connection = InternetHandle(WinHttpConnect(session, host.c_str(), parts.nPort, 0)) };
    if (!open.connection)
    {
        return failed("connect");
    }
    open.request.reset(WinHttpOpenRequest(open.connection.get(), wide(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!open.request)
    {
        return failed("open request");
    }
    return open;
}

auto setTimeouts(void* const session, const HttpTimeouts& timeouts) -> bool
{
    const auto ms = [](const std::chrono::milliseconds t)
    {
        return static_cast<int>(t.count());
    };
    DWORD answer{ static_cast<DWORD>(timeouts.receive.count()) };
    return WinHttpSetTimeouts(session, ms(timeouts.resolve), ms(timeouts.connect), ms(timeouts.send), ms(timeouts.receive)) != FALSE &&
           WinHttpSetOption(session, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &answer, sizeof(answer)) != FALSE;
}

WinHttp::WinHttp(const HttpTimeouts& timeouts)
: session_(openSession())
{
    if (session_ && !setTimeouts(session_.get(), timeouts))
    {
        session_.reset(); // no bound on its requests: refuse them
    }
}

auto WinHttp::request(const std::string& method, const std::string& url, const HttpHeaders& headers, const std::span<const uint8_t> body) -> std::expected<HttpResponse, std::string>
{
    if (!session_)
    {
        return std::unexpected("WinHTTP did not start");
    }
    if (cancelled_)
    {
        return std::unexpected("stopped");
    }
    auto open = openRequest(session_.get(), method, url);
    if (!open)
    {
        return std::unexpected(open.error());
    }
    return perform(open->request.get(), method == "HEAD", headers, body, cancelled_);
}

auto WinHttp::cancel() -> void
{
    cancelled_ = true;
}

} // namespace memoria::plugin
