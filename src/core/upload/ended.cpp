#include "core/upload/uploader.h"

#include <xicap/xicap.h>

#include <format>
#include <optional>

// When Mogsafe ends an upload before it closes: what the 410 says, and the tail of a cut-off capture.
namespace memoria::upload
{
namespace
{

// Writes the packets of source from the first frame that does not end within its first fromByte bytes, as a new
// capture at out that continues source. Returns the packets written, and nothing once stop holds; with none, it writes
// no file.
auto continueFrom(const std::filesystem::path& source, const uint64_t fromByte, const std::filesystem::path& out, const std::function<bool()>& stop) -> std::optional<uint64_t>
{
    xicap::Reader in(source);
    auto          metadata     = in.metadata();
    metadata.previousCaptureId = metadata.captureId;
    metadata.captureId         = {};
    std::optional<xicap::Writer> writer;
    uint64_t                     packets{ 0 };
    xicap::Packet                p;
    while (in.next(p))
    {
        if (stop())
        {
            return std::nullopt;
        }
        if (in.bytesRead() <= fromByte)
        {
            continue;
        }

        if (!writer)
        {
            writer.emplace(out, metadata);
        }

        writer->copy(p);
        packets++;
    }

    if (writer)
    {
        writer->close();
    }

    return packets;
}

} // namespace

// Mogsafe answered 410. A complete capture whose close answer was lost is closed; any other ended upload is dropped,
// and its file never sent again: a recording still running goes on in a new file and upload. Its URL stays: it names
// the recording in Vault, which a session's link still points at.
auto Uploader::applyEnded(UploadFile& f, const HttpResponse& res) -> void
{
    const auto ended = header(res, "mogsafe-ended");
    if (ended == "complete")
    {
        f.stage = Stage::Closed;
    }
    else if (ended == "refused" || ended.empty())
    {
        f.stage = Stage::Ended;
    }
    else
    {
        f.stage = Stage::CutOff;
    }
}

// Once the cut-off file stops changing (the recorder closes it within seconds of the 410), its frames past what Mogsafe
// kept go to <name>-rest.xicap, which uploads as a capture of its own. sent may be below what Mogsafe holds, if an
// append's answer was lost: then a few packets reach Vault twice.
auto Uploader::writeRest(const std::filesystem::path& path, UploadFile& f) -> std::optional<std::string>
{
    if (f.restFailed)
    {
        return std::nullopt;
    }
    std::error_code ec;
    const auto      size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        return ec.message();
    }
    const auto now = std::chrono::steady_clock::now();
    watch(f, size, now);
    if (size > f.sent && now - f.seen < options_.idle)
    {
        return std::nullopt;
    }
    const auto out = path.parent_path() / (path.stem().string() + "-rest.xicap");
    if (size > f.sent && !std::filesystem::exists(out, ec))
    {
        const auto tmp = std::filesystem::path(out).replace_extension(".tmp"); // not .xicap: no pass sends it half written
        try
        {
            const auto packets = continueFrom(path, f.sent, tmp, [this]
                                              {
                                                  return stop_.stop_requested() || finishing_;
                                              });
            if (!packets)
            {
                std::filesystem::remove(tmp, ec);
                return std::nullopt; // stopped: the next run writes the tail
            }
            if (*packets > 0)
            {
                std::filesystem::rename(tmp, out);
            }
        }
        catch (const std::exception& error)
        {
            std::filesystem::remove(tmp, ec);
            f.restFailed = true;
            return std::format("cannot write the cut-off tail: {}", error.what());
        }
    }
    f.stage = Stage::Rescued;
    return std::nullopt;
}

} // namespace memoria::upload
