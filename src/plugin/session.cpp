#include "plugin/session.h"

#include "core/capture/packets.h"
#include "core/install.h"
#include "plugin/client.h"
#include "plugin/text.h"
#include "plugin/winhttp.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace memoria::plugin
{
namespace
{

Session* instance_ = nullptr; // for the search hooks, which take a plain function

// Clients sharing an install take turns through a named mutex. The thread that takes it keeps it until it exits; Windows
// then hands it to the next client. If CreateMutex fails, every client uploads.
auto turn(HANDLE& mutex, const std::string& name) -> std::function<bool()>
{
    if (!mutex)
    {
        mutex = ::CreateMutexA(nullptr, FALSE, name.c_str());
    }
    return [mutex]
    {
        const auto r = mutex ? ::WaitForSingleObject(mutex, 0) : WAIT_OBJECT_0;
        return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    };
}

} // namespace

Session::Session(Host host)
: host_(std::move(host))
, caps_{ host_.caps, host_.captainStop }
{
}

auto Session::start(const std::optional<LateLogin>& late) -> void
{
    server_ = serverIn(host_.config).value_or(server_);

    try
    {
        startUploads();
        calls_    = std::make_unique<cap::Calls>(cap::CallOptions{
                                                     .server      = server_,
                                                     .config      = host_.config,
                                                     .recordingId = [this](const std::string& file)
                                                     {
                                                      return uploader_->recordingId(file);
                                                     },
                                                 },
                                                 std::make_unique<WinHttp>(QUICK_TIMEOUTS));
        recorder_ = std::make_unique<capture::Recorder>(capture::RecorderOptions{
            .dir          = host_.captures,
            .captureAgent = CAPTURE_AGENT,
            .chat         = [this]
            {
                return chatChoice_.chat(linked());
            },
            .ended = [this](const std::filesystem::path& capture)
            {
                if (!uploader_->ended(capture.filename().string()))
                {
                    return false;
                }
                post("Mogsafe ended the last recording. Memoria started a new one.");
                return true;
            },
            .failed = [this](const std::string& why)
            {
                post(paint(Color::Red, "Cannot write the capture: " + why + ".") + " Memoria tries again each minute.");
            },
        });
    }
    catch (const std::exception& error)
    {
        startError_ = error.what();
        say(paint(Color::Red, "Not recording: " + startError_));
        return;
    }
    refreshChat();
    instance_       = this;
    const auto sink = [](xicap::PacketDirection direction, const uint8_t* data, uint32_t size)
    {
        instance_->recordSearch(direction, data, size);
    };
    searchError_ = client::search::install(sink).value_or("");

    if (late)
    {
        enter({ .character = { .characterId = late->characterId, .name = late->name }, .serverKind = client::serverKind(), .midSession = true, .clientVersion = client::lobby::version() });
    }
    host_.log(std::format("Memoria {} recording to {}", MEMORIA_VERSION_TEXT, host_.captures.string()));
}

// NOLINTNEXTLINE(bugprone-exception-escape): only running out of memory throws here
Session::~Session()
{
    cancelLink();
    calls_.reset();
    if (live_)
    {
        closing_.push_back(std::move(live_));
    }
    for (const auto& live : closing_)
    {
        live->abort();
    }
    closing_.clear();
    client::search::remove();
    instance_ = nullptr;
    std::vector<std::string> closed;
    if (recorder_)
    {
        for (const auto& capture : recorder_->stop())
        {
            closed.push_back(capture.filename().string());
        }
        recorder_.reset();
    }
    // Mogsafe ends a silent recording after 5 minutes: without its last bytes and trailer now, it stays cut off.
    if (uploader_ && !closed.empty())
    {
        uploader_->finish(closed, std::chrono::seconds(3));
    }
    uploader_.reset();
    if (uploadMutex_)
    {
        ::CloseHandle(uploadMutex_);
    }
}

auto Session::incoming(const uint16_t id, const uint32_t size, const uint8_t* data, const uint8_t* modified, const uint32_t sizeChunk, const uint8_t* dataChunk, const bool injected, const bool blocked) -> void
{
    if (!recorder_)
    {
        return;
    }
    // A zone-in after character select is a login, and so is one naming another character: a disconnect sends no logout.
    if (id == capture::packets::LOGIN)
    {
        capture::Login who{ .character = capture::packets::character(data, size), .serverKind = client::serverKind(), .clientVersion = client::lobby::version() };
        if (!loggedIn_ || who.character.characterId != loggedInAs_)
        {
            endCap("another character logged in");
            enter(std::move(who));
            refreshChat(); // a change made on Mogsafe applies from the capture after this one
            greet();
            if (live_)
            {
                live_->setHello(hello());
            }
        }
    }
    record(xicap::PacketDirection::ServerToClient, size, data, modified, sizeChunk, dataChunk, injected, blocked);
    if (id == capture::packets::LOGOUT && size > capture::packets::LOGOUT_STATE && capture::packets::leavesGame(data[capture::packets::LOGOUT_STATE]))
    {
        loggedIn_ = false;
        recorder_->logout(clock_.nowMs());
        endCap("you logged out");
    }
}

auto Session::enter(capture::Login who) -> void
{
    characterError_ = client::lobby::fillCharacter(who).value_or("");
    character_      = who.character;
    loggedIn_       = true;
    loggedInAs_     = who.character.characterId;
    recorder_->login(clock_.nowMs(), std::move(who));
}

auto Session::outgoing(const uint32_t size, const uint8_t* data, const uint8_t* modified, const uint32_t sizeChunk, const uint8_t* dataChunk, const bool injected, const bool blocked) -> void
{
    if (recorder_)
    {
        record(xicap::PacketDirection::ClientToServer, size, data, modified, sizeChunk, dataChunk, injected, blocked);
    }
}

auto Session::record(const xicap::PacketDirection direction, const uint32_t size, const uint8_t* data, const uint8_t* modified, const uint32_t sizeChunk, const uint8_t* dataChunk, const bool injected, const bool blocked) -> void
{
    auto provenance = capture::packets::provenance(injected, blocked, data, modified, size);
    // Ashita hands each packet as a copy, so a new chunk shows as a new chunk pointer, size, or head (its sequence number).
    // Windower hands no chunk: there a new chunk shows as a new sync counter, bytes 2-3, which every packet of a chunk shares.
    std::optional<ChunkKey> key;
    if (dataChunk && sizeChunk > 0)
    {
        key = ChunkKey{ .pointer = reinterpret_cast<uintptr_t>(dataChunk), .size = sizeChunk, .head = {} };
        std::memcpy(key->head.data(), dataChunk, (std::min<size_t>)(sizeChunk, key->head.size()));
    }
    else if (size >= 4)
    {
        key = ChunkKey{ .pointer = 0, .size = 0, .head = { data[2], data[3] } };
    }
    if (key)
    {
        auto& last = lastChunk_[std::to_underlying(direction)];
        if (last != key)
        {
            provenance |= xicap::PacketFlag::FirstInDatagram;
        }
        last = key;
    }
    recorder_->packet({
        .timestamp  = clock_.nowMs(),
        .direction  = direction,
        .provenance = provenance,
        .data       = { data, data + size },
    });
}

// The opcode is byte 0x0B, the request type LSB's search server switches on.
auto Session::recordSearch(const xicap::PacketDirection direction, const uint8_t* data, const uint32_t size) -> void
{
    recorder_->packet({
        .timestamp = clock_.nowMs(),
        .direction = direction,
        .channel   = xicap::Channel::Search,
        .opcode    = data[0x0B],
        .data      = { data, data + size },
    });
}

auto Session::frame() -> void
{
    client::search::tick();

    std::vector<std::string> inbox;
    std::optional<Linked>    arrived;
    {
        const std::scoped_lock lock(inboxMutex_);
        inbox.swap(inbox_);
        arrived.swap(arrived_);
    }
    for (const auto& text : inbox)
    {
        say(text);
    }
    if (arrived)
    {
        const auto timeline = character_.name.empty() || character_.world.empty() ? std::string{} : std::format(" Your timeline: {}/timeline/{}/{}", server_, character_.world, character_.name);
        say(paint(Color::Green, std::format("Linked to {}.", arrived->account)) + timeline);
        refreshChat();
        greet();
    }
    if (uploader_ && uploader_->takeUnauthorized())
    {
        sayUnlinked(REFUSED);
    }
    std::erase_if(closing_, [](const auto& l)
                  {
                      return l->finished();
                  });
    if (live_ && live_->finished())
    {
        live_.reset();
    }
}

auto Session::startUploads() -> void
{
    upload::UploadOptions options{
        .dir    = host_.captures,
        .server = server_,
        .config = host_.config,
    };
    options.mayRun = turn(uploadMutex_, std::format("Local\\Memoria-upload-{:x}", std::hash<std::wstring>{}(options.dir.wstring())));
    uploader_      = std::make_unique<upload::Uploader>(std::move(options), std::make_unique<WinHttp>(UPLOAD_TIMEOUTS));
    uploader_->start();
}

auto Session::say(const std::string& text) -> void
{
    host_.say(text);
}

auto Session::post(std::string text) -> void
{
    const std::scoped_lock lock(inboxMutex_);
    inbox_.push_back(std::move(text));
}

// The reminder touches files, so it runs on the worker: a packet hook must not wait on the disk.
auto Session::greet() -> void
{
    if (reminding_.exchange(true))
    {
        return;
    }
    worker_.post([this](const std::stop_token&)
                 {
                     reminding_ = false;
                     remind();
                 });
}

auto Session::remind() -> void
{
    if (!linked() && claimDailyReminder(host_.config))
    {
        post(paint(Color::Yellow, "Your captures stay on this PC.") + " Run " + paint(Color::Cyan, "/memoria link") + " to share them on Mogsafe.");
    }
}

auto Session::link() -> void
{
    cancelLink();
    linking_ = true;
    linker_  = std::jthread([this](const std::stop_token& stop)
                            {
                               waitForLink(stop);
                               linking_ = false;
                            });
}

auto Session::waitForLink(const std::stop_token& stop) -> void
{
    WinHttp http(QUICK_TIMEOUTS);
    // Replacing or destroying linker_ joins this thread on the game thread: the request in flight stops at its next step.
    const std::stop_callback cancel(stop, [&http]
                                    {
                                        http.cancel();
                                    });
    const auto               code = account::startLink(http, server_);
    if (stop.stop_requested())
    {
        return;
    }
    if (!code)
    {
        post(paint(Color::Red, "Cannot start linking: " + code.error()));
        return;
    }
    post("Sign in on the page that opened in your browser and enter " + paint(Color::Yellow, code->code));
    post(code->url);
    if (code->url.starts_with(server_ + "/link"))
    {
        worker_.post([url = code->url](const std::stop_token&)
                     {
                         browse(url);
                     });
    }
    const auto linked = account::waitForApproval(http, server_, *code, stop);
    if (!linked)
    {
        if (!stop.stop_requested())
        {
            post(paint(Color::Red, "Not linked: " + linked.error() + ".") + " Run " + paint(Color::Cyan, "/memoria link") + " again.");
        }
        return;
    }
    const std::scoped_lock link(linkMutex_);
    if (stop.stop_requested())
    {
        return;
    }
    if (const auto error = saveLink(host_.config, *linked))
    {
        post(paint(Color::Red, "Not linked: " + *error + ".") + " Run " + paint(Color::Cyan, "/memoria link") + " again.");
        return;
    }
    const std::scoped_lock inbox(inboxMutex_);
    arrived_ = *linked;
}

// A link thread stopped here saves no token: it checks the stop under linkMutex_.
auto Session::cancelLink() -> bool
{
    {
        const std::scoped_lock link(linkMutex_);
        linker_.request_stop();
    }
    const bool was{ linking_ };
    linker_ = {};
    const std::scoped_lock inbox(inboxMutex_);
    arrived_.reset();
    return was;
}

auto Session::linked() const -> std::optional<Linked>
{
    return loadLink(host_.config);
}

auto Session::sayUnlinked(const std::string& why) -> void
{
    stopLive();
    say(paint(Color::Yellow, why + ".") + " Your captures stay on this PC until you run " + paint(Color::Cyan, "/memoria link") + ".");
}

auto Session::hello() -> std::string
{
    return live::hello(character_.name, character_.world, client::lobby::version(), CAPTURE_AGENT);
}

// One queued request covers every call made before it starts.
auto Session::refreshChat() -> void
{
    if (chatQueued_.exchange(true))
    {
        return;
    }
    worker_.post([this](const std::stop_token& stop)
                 {
                     chatQueued_ = false;
                     WinHttp                  http(QUICK_TIMEOUTS);
                     const std::stop_callback cancel(stop, [&]
                                                     {
                                                         http.cancel();
                                                     });
                     chatChoice_.refresh(http, server_, host_.config);
                 });
}

// Ends the link without waiting: the live thread closes the connection, and a later frame joins it.
auto Session::stopLive() -> void
{
    if (live_)
    {
        live_->stop();
        closing_.push_back(std::move(live_));
    }
}

} // namespace memoria::plugin
