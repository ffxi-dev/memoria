#include "plugin/session.h"

#include "core/units.h"
#include "plugin/client.h"
#include "plugin/text.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace memoria::plugin
{

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
        return chat == xicap::Chat::Keep ? paint(Color::Yellow, "chat saved") : std::string("chat not saved");
    };
    const auto next = chatChoice_.chat(linked());
    client += " · " + chatText(s.capture.empty() ? next : s.chat);
    if (!s.capture.empty() && next != s.chat)
    {
        client += next == xicap::Chat::Keep ? ", saved from the next capture" : ", not saved from the next capture";
    }
    if (s.dropped > 0)
    {
        client += " · " + paint(Color::Yellow, std::format("{} packets dropped", countText(s.dropped)));
    }
    say(client);
    if (const auto& running = caps_.running())
    {
        say(std::format("Capture {} for {}", running->name, cap::lengthText(clock_.nowMs() - running->startMs)) + (s.session.empty() ? "" : ", to " + s.session.filename().string()));
    }
    if (!s.sessionError.empty())
    {
        say(paint(Color::Red, "Capture not saved on this PC: " + s.sessionError));
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
    sayUploads();
}

// "All sent" means Mogsafe holds every byte recorded, whichever client sharing this folder sends it.
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
        say(paint(Color::Yellow, std::format("Not on Mogsafe: {} of {} {} it ended early.", sizeText(kept), cut, cut == 1 ? "recording" : "recordings")) + " It stays on this PC.");
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
    else if (sub == "cap" && arg == "start")
    {
        capStart(restAfter(line, "start"));
    }
    else if (sub == "cap" && arg == "stop")
    {
        capStop();
    }
    else if (sub == "link")
    {
        say("Memoria uploads everything you play on this install while linked, not only your /caps. Anyone can watch it, replay it and download it.");
        if (!std::exchange(privacyShown_, true))
        {
            say("By linking you accept Mogsafe's privacy policy and terms: " + paint(Color::Cyan, server_ + "/privacy") + " · " +
                paint(Color::Cyan, server_ + "/terms"));
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
        say("/memoria status | link | unlink | live [off] | upload on|off | note <text> | cap start [<name>] [<folder>] | cap stop");
    }
}

auto Session::capStart(const std::string_view args) -> void
{
    if (!loggedIn_)
    {
        say("A capture starts once you are in game.");
        return;
    }
    const auto started = caps_.start(*recorder_, utf8FromShiftJis(args), clock_.nowMs(), character_.name);
    if (!started)
    {
        say(paint(Color::Yellow, started.error()));
        return;
    }
    if (!linked())
    {
        say(std::format("Capture {} started.", started->name));
        return;
    }
    calls_->started(*started, character_.characterId);
    say(paused(host_.config) ? std::format("{} will appear on Mogsafe once uploads resume: ", started->name) + paint(Color::Cyan, "/memoria upload on")
                             : "Live at " + cap::page(server_, *started));
}

// Unlinked mid-capture, the stop still goes to Calls, which drops it, so the next capture's calls are not held up.
auto Session::capStop() -> void
{
    const auto now     = clock_.nowMs();
    const auto s       = recorder_->status();
    const auto stopped = caps_.stop(*recorder_, now);
    if (!stopped)
    {
        say("No capture is running.");
        return;
    }
    calls_->stopped(*stopped, now);
    const auto length = cap::lengthText(now - stopped->startMs);
    if (!linked())
    {
        say(s.session.empty() ? std::format("Capture {} ({}) stopped.", stopped->name, length) : std::format("Capture {} ({}) saved to {}", stopped->name, length, s.session.string()));
    }
    else if (paused(host_.config))
    {
        say(std::format("{} ({}) will appear on Mogsafe once uploads resume.", stopped->name, length));
    }
    else
    {
        say(std::format("{} ({}) ready on Mogsafe in about a minute: {}", stopped->name, length, cap::page(server_, *stopped)));
    }
    if (s.session.empty() && !s.sessionError.empty())
    {
        say(paint(Color::Red, "Not saved on this PC: " + s.sessionError));
    }
}

auto Session::endCap(const std::string_view why) -> void
{
    if (const auto ended = caps_.end(*recorder_, clock_.nowMs()))
    {
        calls_->ended(*ended);
        say(paint(Color::Yellow, std::format("Capture {} ended: {}.", ended->name, why)));
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
        say(linked() ? paint(Color::Green, "Uploads on.") : paint(Color::Yellow, "Not linked.") + " Run " + paint(Color::Cyan, "/memoria link") + " to upload.");
        return;
    }
    stopLive();
    say(paint(Color::Yellow, "Uploads paused.") + " " + paint(Color::Cyan, "/memoria upload on") + " resumes them.");
}

} // namespace memoria::plugin
