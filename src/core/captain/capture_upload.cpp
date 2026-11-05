#include "core/captain/importer.h"

#include "core/captain/compress.h"
#include "core/units.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <tuple>

// One captain capture's upload: its CRC and manifest, the stages, the create, the appends and the close.
namespace memoria::captain
{
namespace
{

constexpr size_t MAX_MANIFEST = size_t{ 64 } * 1024; // it goes in a request header; captain's are under 1 KiB
constexpr auto   MAX_WAIT     = std::chrono::milliseconds(std::chrono::minutes(5));

auto base64(const std::string_view in) -> std::string
{
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string                out;
    for (size_t i = 0; i < in.size(); i += 3)
    {
        uint32_t   v{ static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << 16 };
        const auto n = std::min<size_t>(3, in.size() - i);
        if (n > 1)
        {
            v |= static_cast<uint8_t>(in[i + 1]) << 8;
        }
        if (n > 2)
        {
            v |= static_cast<uint8_t>(in[i + 2]);
        }
        for (size_t k = 0; k < 4; ++k)
        {
            out += k <= n ? digits[(v >> (18 - 6 * k)) & 63] : '=';
        }
    }
    return out;
}

auto trim(std::string s) -> std::string
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.'))
    {
        s.pop_back();
    }
    return s;
}

} // namespace

auto Importer::upload(const CaptainCapture& c, const std::string& prefix) -> Outcome
{
    namespace fs   = std::filesystem;
    const auto log = c.dir / "PacketLogger" / "full.log";
    auto       e   = entries_[c.folder];
    if (e.crc.empty() || e.size != c.size)
    {
        const auto crc = crc32File(log, stop_.get_token());
        if (!crc)
        {
            entries_.erase(c.folder);
            if (stop_.stop_requested())
            {
                return Outcome::Stopped;
            }
            post(prefix + "cannot read its full.log; skipped.");
            return Outcome::Skipped;
        }
        e = { .crc = std::format("{:08x}", crc->crc), .size = crc->size };
    }
    std::string manifest(MAX_MANIFEST + 1, '\0');
    {
        std::ifstream in(c.dir / "manifest.txt", std::ios::binary); // closed at once: Windows will not move a folder with a file open
        in.read(manifest.data(), static_cast<std::streamsize>(manifest.size()));
        manifest.resize(static_cast<size_t>(in.gcount()));
    }
    if (manifest.size() > MAX_MANIFEST)
    {
        post(prefix + std::format("its manifest.txt is over {}, more than captain writes; skipped.", sizeText(MAX_MANIFEST)));
        return Outcome::Skipped;
    }

    const auto  metadata = std::format("format {},manifest {},character {},crc32 {},size {},folder {}", base64("captain"), base64(manifest), base64(c.character), base64(e.crc), base64(std::to_string(e.size)), base64(c.folder));
    const auto  staged   = stages(e).log;
    std::string replaces; // the capture this upload gives its search log, as the create said

    auto backoff = options_.retry;
    while (ready())
    {
        const auto again = [&]
        {
            wait(backoff);
            backoff = std::min<std::chrono::milliseconds>(backoff * 2, MAX_WAIT);
        };
        if (e.url.empty())
        {
            if (const auto end = create(c, e, prefix, metadata, replaces))
            {
                return *end;
            }
            if (e.url.empty())
            {
                again();
                continue;
            }
        }

        const auto head = tus_.request("HEAD", e.url, {});
        if (head && (head->status == 404 || head->status == 410))
        {
            // Closed after an answer that was lost, or gone: a new create says which.
            e.url.clear();
            entries_[c.folder] = e;
            std::ignore        = save(); // a lost save costs one more HEAD at the next run
            again();
            continue;
        }
        const auto offset = head && head->status == 200 ? parseU64(header(*head, "upload-offset")) : std::nullopt;
        if (!offset)
        {
            retrying(head ? std::format("offset: {}", head->status) : head.error());
            again();
            continue;
        }

        std::error_code ec;
        if (!fs::exists(staged, ec))
        {
            // Another compression of either log may not match the bytes Mogsafe holds, or the search part's length it was given.
            if (*offset > 0 || (c.searchSize > 0 && !fs::exists(stages(e).search, ec)))
            {
                e.url.clear();
                continue;
            }
            if (const auto error = compressLog(log, staged, c.searchSize > 0 ? stages(e).search : fs::path{}, stop_.get_token()))
            {
                if (stop_.stop_requested())
                {
                    return Outcome::Stopped;
                }
                post(prefix + "cannot stage the log: " + *error + ". Import stopped.");
                return Outcome::Failed;
            }
        }
        const auto total = fs::file_size(staged, ec);
        if (ec)
        {
            e.url.clear(); // the stage went away: a new create stages again
            continue;
        }
        size_ = total;
        sent_ = *offset;
        if (*offset < total)
        {
            const auto sent = tus_.append(staged, e.url, *offset, total, [this](uint64_t at)
                                          {
                                              sent_ = at;
                                              return !stop_.stop_requested() && tus_.allowed();
                                          });
            if (!sent)
            {
                retrying(sent.error());
                again();
                continue;
            }
            sent_ = sent->offset;
            if (sent->offset > *offset)
            {
                backoff = options_.retry;
                retrying({});
            }
            if (sent->answer && sent->answer->status == 410)
            {
                e.url.clear();
                entries_[c.folder] = e;
                std::ignore        = save();
                continue;
            }
            if (sent->answer && sent->answer->status == 422)
            {
                return refuse(c, e, prefix, *sent->answer);
            }
            if (sent->answer)
            {
                retrying(std::format("append: {}", sent->answer->status));
                again();
            }
            continue; // paused, stopped, or done: the next HEAD tells
        }

        constexpr std::string_view body  = "{}";
        const auto                 close = tus_.request("POST", e.url + "/close", { { "Content-Type", "application/json" } }, std::span(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
        if (close && close->status == 200)
        {
            forget(c, e);
            const auto done = [&]() -> std::string
            {
                if (!replaces.empty())
                {
                    return std::format("uploaded with its search log, replacing {}.", replaces);
                }
                if (c.searchSize > 0)
                {
                    return "uploaded with its search log.";
                }
                return "uploaded.";
            }();
            return moveToUploaded(c, prefix, done, Outcome::Uploaded);
        }
        if (close && close->status >= 400 && close->status < 500 && close->status != 409 && close->status != 423)
        {
            return refuse(c, e, prefix, *close);
        }
        retrying(close ? std::format("close: {}", close->status) : close.error());
        again();
    }
    if (!stop_.stop_requested())
    {
        post("Captain import stopped: this install is not linked. It goes on after /memoria link.");
    }
    return Outcome::Stopped;
}

auto Importer::stages(const Entry& e) const -> Stages
{
    const auto dir = options_.state / "imports";
    return { .log = dir / std::format("{}-{}.zst", e.crc, e.size), .search = dir / std::format("{}-{}.search.zst", e.crc, e.size) };
}

// Drops the capture from imports.txt, and its stages with it.
auto Importer::forget(const CaptainCapture& c, const Entry& e) -> void
{
    entries_.erase(c.folder);
    std::ignore = save(); // a line left behind costs one HEAD at the next run
    std::error_code ec;
    std::filesystem::remove(stages(e).log, ec);
    std::filesystem::remove(stages(e).search, ec);
}

auto Importer::refuse(const CaptainCapture& c, const Entry& e, const std::string& prefix, const HttpResponse& res) -> Outcome
{
    forget(c, e);
    post(prefix + std::format("refused: {}.", trim(res.body).empty() ? std::to_string(res.status) : trim(res.body)));
    return Outcome::Refused;
}

// A new create names the logs as they are now, so no stage from an earlier create goes under it. A search log goes ahead
// of full.log in the same upload: it is staged alone here, since the create gives its length. Nothing once created, or
// with e.url still empty when Mogsafe could not be reached; otherwise how the capture ends.
auto Importer::create(const CaptainCapture& c, Entry& e, const std::string& prefix, const std::string& metadata, std::string& replaces) -> std::optional<Outcome>
{
    namespace fs          = std::filesystem;
    const auto      stage = stages(e);
    std::error_code ec;
    fs::remove(stage.log, ec);
    if (!ec && c.searchSize == 0)
    {
        fs::remove(stage.search, ec);
    }
    if (ec)
    {
        post(prefix + "cannot remove an old stage: " + ec.message() + ". Import stopped.");
        return Outcome::Failed;
    }
    std::string search;
    if (c.searchSize > 0)
    {
        const auto staged = stageSearch(c, e, prefix);
        if (!staged)
        {
            return staged.error();
        }
        search = *staged;
    }
    const auto res = tus_.request("POST", "/v1/uploads/", { { "Upload-Defer-Length", "1" }, { "Upload-Metadata", metadata + search } });
    if (res && res->status == 201)
    {
        replaces           = header(*res, "mogsafe-replaces");
        e.url              = tus_.location(*res);
        entries_[c.folder] = e;
        // Saved before the first byte, so a crash resumes this upload. On failure imports.txt stays for the next start or link.
        if (const auto error = save())
        {
            post(prefix + *error + ". Import stopped; it goes on at the next start or link.");
            return Outcome::Stopped;
        }
        return std::nullopt;
    }
    if (res && res->status == 409)
    {
        forget(c, e);
        return moveToUploaded(c, prefix, std::format("already in Vault ({}).", header(*res, "mogsafe-capture")), Outcome::InVault);
    }
    if (res && res->status >= 400 && res->status < 500)
    {
        return refuse(c, e, prefix, *res);
    }
    retrying(res ? std::format("create: {}", res->status) : res.error());
    return std::nullopt;
}

// The metadata that tells the create about the staged search log, or how the capture ends.
auto Importer::stageSearch(const CaptainCapture& c, const Entry& e, const std::string& prefix) -> std::expected<std::string, Outcome>
{
    const auto searchLog = c.dir / "SearchLog" / "search.log";
    const auto staged    = stages(e).search;
    const auto crc       = crc32File(searchLog, stop_.get_token());
    if (!crc)
    {
        if (stop_.stop_requested())
        {
            return std::unexpected(Outcome::Stopped);
        }
        forget(c, e);
        post(prefix + "cannot read its search.log; skipped.");
        return std::unexpected(Outcome::Skipped);
    }
    if (const auto error = compressLog(searchLog, staged, {}, stop_.get_token()))
    {
        if (stop_.stop_requested())
        {
            return std::unexpected(Outcome::Stopped);
        }
        post(prefix + "cannot stage the search log: " + *error + ". Import stopped.");
        return std::unexpected(Outcome::Failed);
    }
    if (const auto staging = crc32File(searchLog, stop_.get_token()); !staging || staging->crc != crc->crc || staging->size != crc->size)
    {
        if (stop_.stop_requested())
        {
            return std::unexpected(Outcome::Stopped);
        }
        forget(c, e);
        post(prefix + "its search.log changed while it was staged; skipped.");
        return std::unexpected(Outcome::Skipped);
    }
    std::error_code ec;
    const auto      bytes = std::filesystem::file_size(staged, ec);
    if (ec)
    {
        post(prefix + "cannot read the search log's stage: " + ec.message() + ". Import stopped.");
        return std::unexpected(Outcome::Failed);
    }
    return std::format(",search_bytes {},search_crc32 {},search_size {}", base64(std::to_string(bytes)), base64(std::format("{:08x}", crc->crc)), base64(std::to_string(crc->size)));
}

auto Importer::moveToUploaded(const CaptainCapture& c, const std::string& prefix, const std::string& done, const Outcome outcome) -> Outcome
{
    const auto to = moveAside(options_.captures, options_.uploaded, c.dir);
    if (!to)
    {
        post(prefix + "in Vault, but its folder could not move: " + to.error() + ".");
        return Outcome::NotMoved;
    }
    post(prefix + done);
    return outcome;
}

} // namespace memoria::captain
