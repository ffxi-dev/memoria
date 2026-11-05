#include "plugin/session.h"

#include "core/file.h"
#include "core/units.h"
#include "plugin/client.h"
#include "plugin/text.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace memoria
{

auto Session::importLine() const -> std::string
{
    const auto p = importer_->progress();
    if (p.index == 0)
    {
        return "Import: looking for captain captures";
    }
    if (p.size == 0)
    {
        return std::format("Import: {}/{} {}, preparing", p.index, p.count, p.character);
    }
    return std::format("Import: {}/{} {}, {}/{} sent", p.index, p.count, p.character, sizeText(p.sent), sizeText(p.size));
}

auto Session::status() -> void
{
    say(paint(Color::Grey, CAPTURE_AGENT));
    const auto s = recorder_->status();
    if (s.capture.empty())
    {
        say(paint(Color::Grey, "Waiting for a login."));
    }
    else
    {
        std::error_code ec;
        const auto      onDisk  = std::filesystem::file_size(s.capture, ec);
        const auto      raw     = s.bytes;
        const auto      seconds = (std::max)(uint64_t{ 1 }, (s.lastMs - s.firstMs) / 1000);
        const auto&     name    = character_.name;
        const auto&     world   = character_.world;
        say(std::format("{} {}{} for {} · {} packets ({:.1f}/s) · {}{}", paint(Color::Green, "Recording"), name, world.empty() ? "" : " on " + world, duration(seconds), count(s.packets), static_cast<double>(s.packets) / static_cast<double>(seconds), sizeText(onDisk), onDisk > 0 ? std::format(" ({:.1f}x smaller)", static_cast<double>(raw) / static_cast<double>(onDisk)) : ""));
    }
    const auto version = client::lobby::version();
    auto       client  = "Client " + (version.empty() ? paint(Color::Grey, "unknown before the first login") : version);
    if (searchError_.empty())
    {
        client += std::format(" · {} search packets", count(s.search));
    }
    client += saveChat_ ? " · " + paint(Color::Yellow, "chat saved") + " (set on Vault)" : " · chat cut";
    if (s.dropped > 0)
    {
        client += " · " + paint(Color::Yellow, std::format("{} packets dropped", count(s.dropped)));
    }
    say(client);
    if (!s.session.empty())
    {
        say("Session: " + s.session.filename().string());
    }
    if (!s.sessionError.empty())
    {
        say(paint(Color::Red, "Session stopped: " + s.sessionError));
    }
    if (!searchError_.empty())
    {
        say(paint(Color::Red, "Search not recorded: " + searchError_));
    }
    if (!characterError_.empty() && loggedIn_)
    {
        say(paint(Color::Grey, "World unknown: " + characterError_));
    }
    if (!s.error.empty())
    {
        say(paint(Color::Red, "Cannot write the capture: " + s.error));
    }
    if (live_)
    {
        say("Live: " + live_->status());
    }
    if (importer_)
    {
        say(importLine());
    }
    const auto link = linked();
    if (!link || account::paused(host_.config))
    {
        say("Uploads: " + paint(Color::Yellow, link ? "paused: /memoria upload on" : "not linked: /memoria link"));
        return;
    }
    // "All sent" means Mogsafe holds every byte recorded so far, whichever client sharing this folder sends it.
    const auto  files = uploader_->status();
    uint64_t    behind{ 0 };
    uint64_t    kept{ 0 }; // in recordings Mogsafe cut off: on this PC only
    int         cut{ 0 };
    const auto* failed = static_cast<const upload::UploadStatus*>(nullptr);
    for (const auto& u : files)
    {
        if (u.refused)
        {
            continue;
        }
        if (u.ended)
        {
            kept += u.size > u.sent ? u.size - u.sent : 0;
            cut += u.size > u.sent ? 1 : 0;
            continue;
        }
        behind += u.size > u.sent ? u.size - u.sent : 0;
        if (!u.error.empty() && failed == nullptr)
        {
            failed = &u;
        }
    }
    const auto account = paint(Color::Grey, " · linked to " + link->account);
    if (failed != nullptr)
    {
        say("Uploads: " + paint(Color::Red, "cannot reach Mogsafe (" + failed->error + "), retrying") + account);
    }
    else if (behind > 0)
    {
        say(std::format("Uploads: catching up, {} to send", sizeText(behind)) + account);
    }
    else
    {
        say("Uploads: " + paint(Color::Green, "all sent") + account);
    }
    if (kept > 0)
    {
        say(paint(Color::Yellow, std::format("Not in Vault: {} of {} {} the server ended early.", sizeText(kept), cut, cut == 1 ? "recording" : "recordings")) + " It stays on this PC.");
    }
    if (const auto error = uploader_->saveError(); !error.empty())
    {
        say(paint(Color::Red, "Cannot save upload progress: " + error));
    }
    for (const auto& u : files | std::views::filter(&upload::UploadStatus::refused))
    {
        say(paint(Color::Red, std::format("Mogsafe refused {}: {}", u.name, u.error)));
    }
}

auto Session::command(const std::vector<std::string>& args, const std::string_view line) -> void
{
    if (!recorder_)
    {
        say("not recording: " + startError_);
        return;
    }
    const auto sub = args.size() > 1 ? args[1] : "status";
    if (sub == "status")
    {
        status();
    }
    else if (sub == "note" && args.size() > 2)
    {
        recorder_->note(clock_.nowMs(), utf8FromShiftJis(args | std::views::drop(2) | std::views::join_with(' ') | std::ranges::to<std::string>()));
        say("noted");
    }
    else if (sub == "session" && args.size() > 2 && args[2] == "start")
    {
        // The rest of the line as typed: a path can hold spaces.
        std::string_view path(line);
        path.remove_prefix(path.find("start") + 5);
        path.remove_prefix((std::min)(path.find_first_not_of(" \""), path.size()));
        path = path.substr(0, path.find_last_not_of(" \"") + 1);
        if (path.empty())
        {
            say("/memoria session start <file.xicap>");
            return;
        }
        // Linked, Vault keeps the session as a range of the recording, from captain's notes: no second file. Unlinked, the file is the only copy.
        if (linked())
        {
            sessionInVault_ = clock_.nowMs();
            say("session recorded in Vault, as part of this recording");
            return;
        }
        recorder_->sessionStart(clock_.nowMs(), std::u8string(path.begin(), path.end()));
        say(std::format("session to {}", path));
    }
    else if (sub == "session" && args.size() > 2 && args[2] == "stop")
    {
        if (const auto from = std::exchange(sessionInVault_, std::nullopt))
        {
            const auto capture = recorder_->status().capture;
            const auto id      = capture.empty() ? std::string{} : uploader_->recordingId(capture.filename().string());
            say(id.empty() ? "Session in Vault once this recording uploads; " + paint(Color::Cyan, "/memoria status") + " shows how far it got."
                           : "Session in Vault: " + paint(Color::Cyan, account::captureLink(server_, id, *from, clock_.nowMs())));
            return;
        }
        recorder_->sessionStop(clock_.nowMs());
        say("session closed");
    }
    else if (sub == "import")
    {
        if (importer_)
        {
            say(importLine());
        }
        else if (!linked())
        {
            say(paint(Color::Yellow, "Not linked.") + " Run " + paint(Color::Cyan, "/memoria link") + " first.");
        }
        else
        {
            createNew(host_.config / "import-reminded");
            startImport(true);
        }
    }
    else if (sub == "link")
    {
        if (!privacyShown_)
        {
            privacyShown_ = true;
            say("The link page asks you to accept Vault's privacy policy: " + paint(Color::Cyan, server_ + "/privacy"));
        }
        say(paint(Color::Grey, "Asking Vault for a link code..."));
        link();
    }
    else if (sub == "live" && args.size() > 2 && args[2] == "off")
    {
        if (live_)
        {
            stopLive();
            say(paint(Color::Yellow, "Live off.") + " The link has ended.");
        }
        else
        {
            say("Not live.");
        }
    }
    else if (const auto link = linked(); sub == "live" && !link)
    {
        say(paint(Color::Yellow, "Not linked.") + " Run " + paint(Color::Cyan, "/memoria link") + " first.");
    }
    else if (sub == "live" && account::paused(host_.config))
    {
        say(paint(Color::Yellow, "Uploads are paused.") + " Run " + paint(Color::Cyan, "/memoria upload on") + " first.");
    }
    else if (sub == "live" && live_)
    {
        live_->reopen();
        say("Opening the live link again.");
    }
    else if (sub == "live")
    {
        live_ = std::make_unique<Live>(server_, host_.config, link->token, recorder_->live(), hello(), [this](std::string text)
                                       {
                                           post(std::move(text));
                                       });
        say(paint(Color::Grey, "Asking Vault for a live link..."));
    }
    else if (sub == "unlink" && linked())
    {
        std::error_code ec;
        if (!std::filesystem::remove(host_.config / "token", ec) && ec)
        {
            say(paint(Color::Red, "Still linked: cannot delete " + (host_.config / "token").string() + ": " + ec.message()));
            return;
        }
        saveChat_ = false; // an unlinked install never saves chat
        unlink("Unlinked");
    }
    else if (sub == "upload" && args.size() > 2 && (args[2] == "off" || args[2] == "on"))
    {
        const auto      pausedFile = host_.config / "paused";
        std::error_code ec;
        if (args[2] == "off")
        {
            if (!std::ofstream{ pausedFile })
            {
                say(paint(Color::Red, "Uploads still on: cannot create " + pausedFile.string()));
                return;
            }
            stopLive();
            say(paint(Color::Yellow, "Uploads paused.") + " " + paint(Color::Cyan, "/memoria upload on") + " resumes them.");
        }
        else
        {
            if (!std::filesystem::remove(pausedFile, ec) && ec)
            {
                say(paint(Color::Red, "Uploads still paused: cannot delete " + pausedFile.string() + ": " + ec.message()));
                return;
            }
            say(linked() ? paint(Color::Green, "Uploads on.") : paint(Color::Yellow, "Uploads off: not linked: /memoria link"));
        }
    }
    else
    {
        say("/memoria status | link | unlink | live [off] | upload on|off | import | note <text> | session start <file.xicap> | session stop");
    }
}

} // namespace memoria
