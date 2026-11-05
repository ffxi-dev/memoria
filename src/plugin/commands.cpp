#include "plugin/session.h"

#include "core/units.h"
#include "plugin/client.h"
#include "plugin/text.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace memoria::plugin
{
namespace
{

// A recording's page on Mogsafe, opened on the range fromMs to toMs (Unix ms), as Mogsafe's capture page reads ?from=&to=.
auto recordingPage(const std::string& server, const std::string& id, const uint64_t fromMs, const uint64_t toMs) -> std::string
{
    return std::format("{}/captures/{}?from={}&to={}", server, id, fromMs, toMs);
}

} // namespace

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
    return std::format("Import: {}/{} {}, {}/{} sent{}", p.index, p.count, p.character, sizeText(p.sent), sizeText(p.size), p.error.empty() ? "" : ", retrying: " + p.error);
}

auto Session::sayStatus() -> void
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
        const auto      seconds = (std::max)(uint64_t{ 1 }, (s.lastMs - s.firstMs) / 1000);
        say(std::format("{} {}{} for {} · {} packets ({:.1f}/s) · {}{}", paint(Color::Green, "Recording"), character_.name, character_.world.empty() ? "" : " on " + character_.world, durationText(seconds), countText(s.packets), static_cast<double>(s.packets) / static_cast<double>(seconds), sizeText(onDisk), onDisk > 0 ? std::format(" ({:.1f}x smaller)", static_cast<double>(s.bytes) / static_cast<double>(onDisk)) : ""));
    }
    const auto version = client::lobby::version();
    auto       client  = "Client " + (version.empty() ? paint(Color::Grey, "unknown before the first login") : version);
    if (searchError_.empty())
    {
        client += std::format(" · {} search packets", countText(s.search));
    }
    const auto chatText = [](const xicap::Chat chat)
    {
        return chat == xicap::Chat::Keep ? paint(Color::Yellow, "chat saved") + " (set on Mogsafe)" : std::string("chat cut");
    };
    const auto next = chatChoice_.chat(linked());
    client += " · " + chatText(s.capture.empty() ? next : s.chat);
    if (!s.capture.empty() && next != s.chat)
    {
        client += " (the next capture: " + chatText(next) + ")";
    }
    if (s.dropped > 0)
    {
        client += " · " + paint(Color::Yellow, std::format("{} packets dropped", countText(s.dropped)));
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
    sayUploads();
}

// "All sent" means Mogsafe holds every byte recorded so far, whichever client sharing this folder sends it.
auto Session::sayUploads() -> void
{
    const auto link = linked();
    if (!link || paused(host_.config))
    {
        say("Uploads: " + paint(Color::Yellow, link ? "paused: /memoria upload on" : "not linked: /memoria link"));
        return;
    }
    const auto files                               = uploader_->status();
    const auto [behind, kept, cut, foreign, error] = upload::totals(files);
    const auto account                             = paint(Color::Grey, " · linked to " + link->account);
    if (!error.empty())
    {
        say("Uploads: " + paint(Color::Red, "retrying, " + error) + account);
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
        say(paint(Color::Yellow, std::format("Not on Mogsafe: {} of {} {} the server ended early.", sizeText(kept), cut, cut == 1 ? "recording" : "recordings")) + " It stays on this PC.");
    }
    if (foreign > 0)
    {
        say(paint(Color::Yellow, std::format("Not sent: {} {} uploaded under another account.", foreign, foreign == 1 ? "recording" : "recordings")) + " Link that account to send the rest.");
    }
    if (const auto saveError = uploader_->saveError(); !saveError.empty())
    {
        say(paint(Color::Red, "Cannot save upload progress: " + saveError));
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
        say(paint(Color::Red, "Not recording: " + startError_));
        return;
    }
    const auto sub = args.size() > 1 ? args[1] : "status";
    const auto arg = args.size() > 2 ? args[2] : "";
    if (sub == "status")
    {
        sayStatus();
    }
    else if (sub == "note" && !arg.empty())
    {
        recorder_->note(clock_.nowMs(), utf8FromShiftJis(args | std::views::drop(2) | std::views::join_with(' ') | std::ranges::to<std::string>()));
        say("Noted.");
    }
    else if (sub == "session" && arg == "start")
    {
        startSession(restAfter(line, "start"));
    }
    else if (sub == "session" && arg == "stop")
    {
        stopSession();
    }
    else if (sub == "import")
    {
        importCaptain();
    }
    else if (sub == "link")
    {
        if (!std::exchange(privacyShown_, true))
        {
            say("The link page asks you to accept Mogsafe's privacy policy: " + paint(Color::Cyan, server_ + "/privacy"));
        }
        say(paint(Color::Grey, "Asking Mogsafe for a link code..."));
        link();
    }
    else if (sub == "live" && arg == "off")
    {
        say(live_ ? paint(Color::Yellow, "Live off.") + " The link has ended." : "Not live.");
        stopLive();
    }
    else if (sub == "live")
    {
        goLive();
    }
    else if (sub == "unlink")
    {
        unlinkInstall();
    }
    else if (sub == "upload" && (arg == "off" || arg == "on"))
    {
        setUploads(arg == "on");
    }
    else
    {
        say("/memoria status | link | unlink | live [off] | upload on|off | import | note <text> | session start <file.xicap> | session stop");
    }
}

// Linked, Vault keeps the session as a range of the recording, from captain's notes: no second file. Unlinked, the file is
// the only copy.
auto Session::startSession(const std::string_view path) -> void
{
    if (path.empty())
    {
        say("/memoria session start <file.xicap>");
        return;
    }
    if (linked())
    {
        sessionInVault_ = VaultSession{ .fromMs = clock_.nowMs(), .capture = recorder_->status().capture };
        say("Session recorded on Mogsafe, as part of this recording.");
        return;
    }
    try
    {
        const auto utf8 = utf8FromShiftJis(std::string(path)); // the chat line is Shift-JIS
        recorder_->sessionStart(clock_.nowMs(), std::filesystem::path(std::u8string(utf8.begin(), utf8.end())));
    }
    catch (const std::exception& error)
    {
        say(paint(Color::Red, std::format("No session: {}", error.what())));
        return;
    }
    say(std::format("Session to {}.", path));
}

auto Session::stopSession() -> void
{
    const auto from = std::exchange(sessionInVault_, std::nullopt);
    if (!from)
    {
        if (recorder_->status().session.empty())
        {
            say("No session is open.");
            return;
        }
        recorder_->sessionStop(clock_.nowMs());
        say("Session closed.");
        return;
    }
    const auto now     = clock_.nowMs();
    const auto current = recorder_->status();
    const auto first   = from->capture.empty() ? current.capture : from->capture;
    const auto sayLink = [&](const std::filesystem::path& capture, const uint64_t fromMs)
    {
        const auto id = capture.empty() ? std::string{} : uploader_->recordingId(capture.filename().string());
        say(id.empty() ? "Session on Mogsafe once this recording uploads; " + paint(Color::Cyan, "/memoria status") + " shows how far it got."
                       : "Session on Mogsafe: " + paint(Color::Cyan, recordingPage(server_, id, fromMs, now)));
    };
    sayLink(first, from->fromMs);
    if (!current.capture.empty() && current.capture != first)
    {
        say("It went on in a new recording:");
        sayLink(current.capture, current.firstMs);
    }
}

auto Session::importCaptain() -> void
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
        std::ignore = markImportReminded(host_.config);
        startImport(true);
    }
}

auto Session::goLive() -> void
{
    const auto link = linked();
    if (!link)
    {
        say(paint(Color::Yellow, "Not linked.") + " Run " + paint(Color::Cyan, "/memoria link") + " first.");
        return;
    }
    if (paused(host_.config))
    {
        say(paint(Color::Yellow, "Uploads are paused.") + " Run " + paint(Color::Cyan, "/memoria upload on") + " first.");
        return;
    }
    if (live_)
    {
        live_->reopen();
        say("Opening the live link again.");
        return;
    }
    live_ = std::make_unique<Live>(LiveOptions{
                                       .server       = server_,
                                       .config       = host_.config,
                                       .installToken = link->token,
                                       .hello        = hello(),
                                       .post         = [this](std::string text)
                                       {
                                           post(std::move(text));
                                       },
                                       .open = [this](std::string url)
                                       {
                                           worker_.post([url = std::move(url)](const std::stop_token&)
                                                        {
                                                            browse(url);
                                                        });
                                       },
                                   },
                                   recorder_->live());
    say(paint(Color::Grey, "Asking Mogsafe for a live link..."));
}

auto Session::unlinkInstall() -> void
{
    const bool linking = cancelLink();
    if (!linked())
    {
        say(linking ? "Linking stopped." : "Not linked.");
        return;
    }
    if (const auto error = unlink(host_.config))
    {
        say(paint(Color::Red, "Still linked: " + *error));
        return;
    }
    sayUnlinked("Unlinked");
}

auto Session::setUploads(const bool on) -> void
{
    if (const auto error = setPaused(host_.config, !on))
    {
        say(paint(Color::Red, std::format("Uploads still {}: {}", on ? "paused" : "on", *error)));
        return;
    }
    if (on)
    {
        say(linked() ? paint(Color::Green, "Uploads on.") : paint(Color::Yellow, "Uploads off: not linked: /memoria link"));
        return;
    }
    stopLive();
    say(paint(Color::Yellow, "Uploads paused.") + " " + paint(Color::Cyan, "/memoria upload on") + " resumes them.");
}

} // namespace memoria::plugin
