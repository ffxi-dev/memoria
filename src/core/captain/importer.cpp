#include "core/captain/importer.h"

#include "core/file.h"
#include "core/install.h"
#include "core/units.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace memoria::captain
{
namespace
{

constexpr auto STATE_FILE = "imports.txt";
} // namespace

Importer::Importer(ImportOptions options, std::unique_ptr<Http> http)
: options_(std::move(options))
, http_(std::move(http))
, tus_(options_.server, options_.config, *http_)
{
}

Importer::~Importer()
{
    {
        const std::scoped_lock lock(mutex_);
        stop_.request_stop();
    }
    http_->cancel();
    wake_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

auto Importer::pending(const std::filesystem::path& state) -> bool
{
    std::error_code ec;
    return std::filesystem::exists(state / STATE_FILE, ec);
}

auto Importer::start(const bool asked) -> void
{
    thread_ = std::thread([this, asked]
                          {
#ifdef _WIN32
                              ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
                              if (options_.mayRun && !options_.mayRun())
                              {
                                  if (asked)
                                  {
                                      say("Another client sharing this install is importing.");
                                  }
                                  finished_ = true;
                                  return;
                              }
                              try
                              {
                                  run();
                              }
                              catch (const std::exception& error)
                              {
                                  say(std::format("Captain import stopped: {}", error.what()));
                              }
                              finished_ = true;
                          });
}

auto Importer::run() -> void
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(options_.state, ec);
    load();
    if (const auto error = save()) // its existence marks the run as asked for until it ends
    {
        say(std::format("Captain import stopped: {}.", *error));
        return;
    }

    auto found = findCaptures(options_.captures, options_.quiet, stop_.get_token());
    if (stop_.stop_requested())
    {
        return;
    }
    count_ = found.size();
    if (found.empty())
    {
        say("No captain captures to upload.");
    }
    else
    {
        uint64_t total{ 0 };
        for (const auto& c : found)
        {
            total += c.growing ? 0 : c.size + c.searchSize;
        }
        say(std::format("Uploading {} captain capture{} ({}) in the background.", found.size(), found.size() == 1 ? "" : "s", sizeText(total)));
    }

    std::map<Outcome, size_t> tally;
    for (size_t i = 0; i < found.size(); ++i)
    {
        const auto& c = found[i];
        {
            const std::scoped_lock lock(mutex_);
            character_ = c.character;
            error_.clear();
        }
        index_            = i + 1;
        sent_             = 0;
        size_             = 0;
        const auto prefix = std::format("Captain {}/{} {}: ", i + 1, found.size(), c.folder);
        if (c.growing)
        {
            say(prefix + "still being written; skipped.");
            ++tally[Outcome::Skipped];
            continue;
        }
        const auto result = upload(c, prefix);
        if (result == Outcome::Stopped || result == Outcome::Failed)
        {
            return; // imports.txt stays, so the next start or link resumes
        }
        ++tally[result];
    }

    std::string summary{ std::format("Captain import done: {} uploaded, {} already in Vault, {} refused", tally[Outcome::Uploaded], tally[Outcome::InVault], tally[Outcome::Refused]) };
    if (tally[Outcome::Skipped] > 0)
    {
        summary += std::format(", {} skipped", tally[Outcome::Skipped]);
    }
    if (tally[Outcome::NotMoved] > 0)
    {
        summary += std::format(", {} in Vault but not moved", tally[Outcome::NotMoved]);
    }
    if (!found.empty())
    {
        say(summary + ". Uploaded folders are in addons/captain/captures-uploaded.");
    }
    fs::remove(options_.state / STATE_FILE, ec);
    fs::remove(options_.state / "imports", ec); // only once empty
}

auto Importer::ready() -> bool
{
    while (!stop_.stop_requested())
    {
        if (tus_.allowed())
        {
            return true;
        }
        // The token decides, not the paused file, which /memoria upload on may delete between the two reads.
        if (!loadLink(options_.config))
        {
            return false;
        }
        wait(options_.retry);
    }
    return false;
}

auto Importer::wait(const std::chrono::milliseconds time) -> void
{
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, time, [this]
                   {
                       return stop_.stop_requested();
                   });
}

auto Importer::retrying(std::string why) -> void
{
    const std::scoped_lock lock(mutex_);
    error_ = std::move(why);
}

auto Importer::say(const std::string& text) -> void
{
    if (options_.say)
    {
        options_.say(text);
    }
}

// One line per capture in flight: folder, CRC-32, size, and upload URL, separated by tabs.
auto Importer::load() -> void
{
    std::ifstream in(options_.state / STATE_FILE);
    for (std::string line; std::getline(in, line);)
    {
        std::istringstream fields(line);
        std::string        folder;
        std::string        crc;
        std::string        size;
        std::string        url;
        if (std::getline(fields, folder, '\t') && std::getline(fields, crc, '\t') && std::getline(fields, size, '\t') && std::getline(fields, url))
        {
            entries_[folder] = { .crc = crc, .size = parseU64(size).value_or(0), .url = url };
        }
    }
}

auto Importer::save() -> std::optional<std::string>
{
    std::string text;
    for (const auto& [folder, e] : entries_)
    {
        if (!e.url.empty())
        {
            text += std::format("{}\t{}\t{}\t{}\n", folder, e.crc, e.size, e.url);
        }
    }
    return replaceFile(options_.state / STATE_FILE, text);
}

auto Importer::finished() const -> bool
{
    return finished_;
}

auto Importer::progress() const -> ImportProgress
{
    const std::scoped_lock lock(mutex_);
    return { .index = index_, .count = count_, .character = character_, .sent = sent_, .size = size_, .error = error_ };
}

auto Importer::unauthorized() -> bool
{
    return tus_.unauthorized();
}

} // namespace memoria::captain
