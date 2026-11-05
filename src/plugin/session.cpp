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
// then hands it to the next client. Without a mutex, every client may run.
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
{
}

auto Session::start(const std::optional<LateLogin>& late) -> void
{
    server_ = serverIn(host_.config).value_or(server_);

    try
    {
        startUploads();
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
                post("The server ended the last recording; continuing in a new one.");
                return true;
            },
            .failed = [this](const std::string& why)
            {
                post(paint(Color::Red, "Cannot write the capture: " + why + ".") + " Nothing records until it can; Memoria tries again each minute.");
            },
        });
        if (captain::Importer::pending(host_.captures) && linked())
        {
            startImport(false); // an import asked for and not finished goes on, here or in the client holding its mutex
        }
    }
    catch (const std::exception& error)
    {
        startError_ = error.what();
        say("not recording: " + startError_);
        return;
    }
    refreshChat();
    instance_    = this;
    searchError_ = client::search::install([](bool clientToServer, const uint8_t* data, uint32_t size)
                                           {
                                               instance_->recordSearch(clientToServer, data, size);
                                           });

    // Loaded while already in a zone: record from here, marked mid-session.
    if (late)
    {
        capture::Login login{ .character = { .characterId = late->characterId, .name = late->name }, .serverKind = client::serverKind(), .midSession = true, .clientVersion = client::lobby::version() };
        characterError_ = client::lobby::character(login);
        character_      = login.character;
        loggedIn_       = true;
        loggedInAs_     = login.character.characterId;
        recorder_->login(clock_.nowMs(), std::move(login));
    }
    host_.log(std::format("Memoria {} recording to {}", MEMORIA_VERSION_TEXT, host_.captures.string()));
}

// NOLINTNEXTLINE(bugprone-exception-escape): only running out of memory throws here
Session::~Session()
{
    cancelLink();
    importer_.reset();
    if (live_)
    {
        live_->abort();
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
    // Mogsafe ends a silent recording after 5 minutes: without its last bytes and trailer now, Vault keeps it cut off.
    if (uploader_ && !closed.empty())
    {
        uploader_->finish(closed, std::chrono::seconds(3));
    }
    uploader_.reset();
    for (auto* const mutex : { uploadMutex_, importMutex_ })
    {
        if (mutex)
        {
            ::CloseHandle(mutex);
        }
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
            characterError_ = client::lobby::character(who);
            character_      = who.character;
            loggedIn_       = true;
            loggedInAs_     = who.character.characterId;
            refreshChat(); // a change made on Mogsafe applies from the capture after this one
            greet();
            recorder_->login(clock_.nowMs(), std::move(who));
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
    }
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
    uint16_t provenance{ 0 };
    if (injected)
    {
        provenance |= xicap::PacketFlag::Injected;
    }
    if (blocked)
    {
        provenance |= xicap::PacketFlag::Blocked;
    }
    if (modified && std::memcmp(data, modified, size) != 0)
    {
        provenance |= xicap::PacketFlag::Modified;
    }
    // Ashita hands each packet as a copy, so a new chunk shows as a new chunk pointer, size, or head (its sequence number).
    if (dataChunk && sizeChunk > 0)
    {
        ChunkKey key{ .pointer = reinterpret_cast<uintptr_t>(dataChunk), .size = sizeChunk, .head = {} };
        std::memcpy(key.head.data(), dataChunk, (std::min<size_t>)(sizeChunk, key.head.size()));
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
auto Session::recordSearch(const bool clientToServer, const uint8_t* data, const uint32_t size) -> void
{
    recorder_->packet({
        .timestamp = clock_.nowMs(),
        .direction = clientToServer ? xicap::PacketDirection::ClientToServer : xicap::PacketDirection::ServerToClient,
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
        say(paint(Color::Green, std::format("Linked to {}.", arrived->account)) + " Your captures upload from now on.");
        refreshChat();
        if (!importer_ && captain::Importer::pending(host_.captures))
        {
            startImport(false);
        }
        greet();
    }
    if (importer_ && importer_->unauthorized())
    {
        unlinked("Vault no longer accepts this install");
    }
    if (importer_ && importer_->finished())
    {
        importer_.reset();
    }
    std::erase_if(closing_, [](const auto& l)
                  {
                      return l->finished();
                  });
    if (live_ && live_->finished())
    {
        live_.reset();
    }
    if (uploader_ && uploader_->unauthorized())
    {
        unlinked("Vault no longer accepts this install");
    }
}

// Runs until the plugin unloads; the token and paused files decide each pass whether it sends.
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

// At a login or link. The reminders read and write files, so they run on the worker: a packet hook never waits on the disk.
auto Session::greet() -> void
{
    if (reminding_.exchange(true))
    {
        return;
    }
    worker_.post([this](const std::stop_token& stop)
                 {
                     reminding_ = false;
                     remind();
                     offerImport(stop);
                 });
}

// Once a day while unlinked: the date of the last reminder is kept across restarts.
auto Session::remind() -> void
{
    const auto  path  = host_.config / "reminded";
    const auto  today = std::format("{:%F}", std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
    std::string last;
    std::getline(std::ifstream(path), last);
    if (linked() || last == today)
    {
        return;
    }
    std::ofstream(path, std::ios::trunc) << today;
    post(paint(Color::Yellow, "Your captures stay on this PC.") + " Run " + paint(Color::Cyan, "/memoria link") + " to share them with Vault.");
}

// Once per install, ever: while linked and captain captures wait.
auto Session::offerImport(const std::stop_token& stop) -> void
{
    // Clients logging in together race to create the reminded file; only the winner speaks.
    if (linked() && captain::remindImport(host_.config, host_.captain / "captures", stop))
    {
        post("Run " + paint(Color::Cyan, "/memoria import") + " to upload existing Captain captures.");
    }
}

// Clients sharing an install import through the one whose importer holds the named mutex.
auto Session::startImport(const bool asked) -> void
{
    captain::ImportOptions options{
        .captures = host_.captain / "captures",
        .uploaded = host_.captain / "captures-uploaded",
        .state    = host_.captures,
        .server   = server_,
        .config   = host_.config,
        .mayRun   = turn(importMutex_, std::format("Local\\Memoria-import-{:x}", std::hash<std::wstring>{}((host_.captain / "captures").wstring()))),
    };
    options.say = [this](std::string text)
    {
        post(std::move(text));
    };
    importer_ = std::make_unique<captain::Importer>(std::move(options), std::make_unique<WinHttp>(UPLOAD_TIMEOUTS));
    importer_->start(asked);
}

// Asks for a code, then polls on a thread of its own until the player approves it on the link page.
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

// Ends a link being made: its request is cancelled, and a token it saves later than this never is.
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

auto Session::unlinked(const std::string& why) -> void
{
    stopLive();
    say(paint(Color::Yellow, why + ".") + " Your captures stay on this PC until you run " + paint(Color::Cyan, "/memoria link") + ".");
}

auto Session::hello() -> std::string
{
    return live::hello(character_.name, character_.world, client::lobby::version(), CAPTURE_AGENT);
}

// A linked install saves chat when its account chose to. The request runs on the worker; one queued covers every call
// made before it starts.
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
