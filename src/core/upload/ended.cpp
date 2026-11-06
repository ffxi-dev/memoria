#include "core/upload/uploader.h"

#include <xicap/xicap.h>

#include <format>
#include <optional>

// A 410: Mogsafe ended an upload before it closed.
namespace memoria::upload
{
namespace
{

// Copies source's packets from the first frame ending past fromByte into a new capture at out that continues source.
// Returns the packet count, or nothing if stopped. Writes no file for zero packets.
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

// A complete capture whose close answer was lost counts as closed; any other ended file is not sent again. The URL
// stays: it names the recording a session's link points at.
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

// Once the cut-off file stops changing (the recorder closes it within seconds of the 410), its frames past sent go to
// <name>-rest.xicap, a capture of its own. If an append's answer was lost, sent trails Mogsafe and a few packets reach
// Mogsafe twice.
auto Uploader::writeRest(const std::filesystem::path& path, UploadFile& f) -> std::optional<std::string>
{
    if (f.restFailed)
    {
        return std::nullopt;
    }
    const auto watched = watch(path, f);
    if (!watched)
    {
        return watched.error();
    }
    const auto      size = *watched;
    std::error_code ec;
    if (size > f.sent && std::chrono::steady_clock::now() - f.seen < options_.idle)
    {
        return std::nullopt;
    }
    const auto out = path.parent_path() / (path.stem().string() + "-rest.xicap");
    if (size > f.sent && !std::filesystem::exists(out, ec))
    {
        const auto tmp = std::filesystem::path(out).replace_extension(".tmp"); // not .xicap, so no pass sends it half written
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
