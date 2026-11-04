#include "plugin/session.h"

#include "core/file.h"
#include "plugin/client.h"
#include "plugin/packets.h"
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

namespace memoria
{
namespace
{

// upload.ini's "server = https://..." line, which points Memoria at another Mogsafe.
auto serverIn(const std::filesystem::path& ini) -> std::optional<std::string>
{
    const auto trim = [](std::string_view s)
    {
        const auto first = s.find_first_not_of(" \t\r");
        return first == std::string_view::npos ? std::string{} : std::string(s.substr(first, s.find_last_not_of(" \t\r/") - first + 1));
    };
    std::ifstream in(ini);
    for (std::string line; std::getline(in, line);)
    {
        const auto eq = line.find('=');
        if (eq != std::string::npos && trim(line.substr(0, eq)) == "server" && trim(line.substr(eq + 1)).starts_with("https://"))
        {
            return trim(line.substr(eq + 1));
        }
    }
    return std::nullopt;
}

Session* instance_ = nullptr; // for the search hooks, which take a plain function

} // namespace

Session::Session(Host host)
: host_(std::move(host))
{
}

auto Session::start(std::optional<LateLogin> late) -> void
{
    server_ = serverIn(host_.config / "upload.ini").value_or(server_);

    try
    {
        startUploads();
        recorder_ = std::make_unique<Recorder>(RecorderOptions{
            .dir          = host_.captures,
            .captureAgent = CAPTURE_AGENT,
            .policy       = [this]
            {
                return policyFor(saveChat_);
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
        if (Importer::pending(host_.captures) && linked())
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
        Login login{ .character = { .characterId = late->characterId, .name = late->name }, .network = client::network(), .midSession = true, .clientVersion = client::lobby::version() };
        characterError_ = client::lobby::character(login);
        character_      = login.character;
        loggedIn_       = true;
        loggedInAs_     = login.character.characterId;
        recorder_->login(clock_.nowMs(), std::move(login));
    }
    host_.log(std::format("Memoria {} recording to {}", MEMORIA_VERSION_TEXT, host_.captures.string()));
}

Session::~Session()
{
    linker_    = {}; // stops and joins
    chatFetch_ = {};
    reminder_  = {};
    importer_.reset(); // cancels its request and joins
    stopLive();
    closing_.clear(); // joins
    client::search::remove();
    std::filesystem::path capture;
    if (recorder_)
    {
        capture = recorder_->status().capture;
        recorder_->stop(); // writes the trailer
        recorder_.reset();
    }
    // Mogsafe ends a silent recording after 5 minutes: without its last bytes and trailer now, Vault keeps it cut off.
    if (uploader_ && !capture.empty())
    {
        uploader_->finish(capture.filename().string(), std::chrono::seconds(3));
    }
    uploader_.reset();
    if (uploadMutex_)
    {
        ::CloseHandle(std::exchange(uploadMutex_, nullptr));
    }
}

auto Session::incoming(uint16_t id, uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void
{
    if (!recorder_)
    {
        return;
    }
    // A zone-in after character select is a login, and so is one naming another character: a disconnect sends no logout.
    if (id == packets::LOGIN)
    {
        Login who{ .character = packets::character(data, size), .network = client::network(), .clientVersion = client::lobby::version() };
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
    record(false, size, data, modified, sizeChunk, dataChunk, injected, blocked);
    if (id == packets::LOGOUT && size > packets::LOGOUT_STATE && packets::leavesGame(data[packets::LOGOUT_STATE]))
    {
        loggedIn_ = false;
        recorder_->logout(clock_.nowMs());
    }
}

auto Session::outgoing(uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void
{
    if (recorder_)
    {
        record(true, size, data, modified, sizeChunk, dataChunk, injected, blocked);
    }
}

auto Session::record(bool clientToServer, uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void
{
    uint16_t provenance = 0;
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
        ChunkKey key{ reinterpret_cast<uintptr_t>(dataChunk), sizeChunk, {} };
        std::memcpy(key.head.data(), dataChunk, (std::min<size_t>)(sizeChunk, key.head.size()));
        auto& last = lastChunk_[clientToServer ? 1 : 0];
        if (last != key)
        {
            provenance |= xicap::PacketFlag::FirstInChunk;
        }
        last = key;
    }
    recorder_->packet({
        .timestamp      = clock_.nowMs(),
        .clientToServer = clientToServer,
        .provenance     = provenance,
        .data           = { data, data + size },
    });
}

// The opcode is byte 0x0B, the request type LSB's search server switches on.
auto Session::recordSearch(bool clientToServer, const uint8_t* data, uint32_t size) -> void
{
    recorder_->packet({
        .timestamp      = clock_.nowMs(),
        .clientToServer = clientToServer,
        .channel        = std::to_underlying(xicap::Channel::Search),
        .opcode         = data[0x0B],
        .data           = { data, data + size },
    });
}

auto Session::frame() -> void
{
    client::search::tick();

    std::vector<std::string> inbox;
    std::optional<Linked>    arrived;
    {
        std::lock_guard lock(inboxMutex_);
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
        if (!importer_ && Importer::pending(host_.captures))
        {
            startImport(false);
        }
        greet();
    }
    if (importer_ && importer_->unauthorized())
    {
        unlink("Vault no longer accepts this install");
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
        unlink("Vault no longer accepts this install");
    }
}

// Runs until the plugin unloads; the token and paused files decide each pass whether it sends.
auto Session::startUploads() -> void
{
    UploadOptions options{
        .dir    = host_.captures,
        .server = server_,
        .config = host_.config,
    };
    // Clients sharing an install share its captures folder; the one whose upload thread holds this mutex uploads, and Windows hands it on when that client exits.
    if (!uploadMutex_)
    {
        uploadMutex_ = ::CreateMutexA(nullptr, FALSE, std::format("Local\\Memoria-upload-{:x}", std::hash<std::wstring>{}(options.dir.wstring())).c_str());
    }
    options.mayRun = [mutex = uploadMutex_]
    {
        const auto r = ::WaitForSingleObject(mutex, 0);
        return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    };
    uploader_ = std::make_unique<Uploader>(std::move(options), std::make_unique<WinHttp>());
    uploader_->start();
}

auto Session::say(const std::string& text) -> void
{
    host_.say(text);
}

auto Session::post(std::string text) -> void
{
    std::lock_guard lock(inboxMutex_);
    inbox_.push_back(std::move(text));
}

// At a login or link. The reminders read and write files, so they run on a thread of their own: a packet hook never waits on the disk.
auto Session::greet() -> void
{
    if (reminding_.exchange(true))
    {
        return;
    }
    reminder_ = std::jthread([this]
                             {
                                 remind();
                                 offerImport();
                                 reminding_ = false;
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
auto Session::offerImport() -> void
{
    // Clients logging in together race to create the reminded file; only the winner speaks.
    if (linked() && remindImport(host_.config, host_.captain / "captures"))
    {
        post("Run " + paint(Color::Cyan, "/memoria import") + " to upload existing Captain captures.");
    }
}

// Clients sharing an install import through the one whose importer holds the named mutex.
auto Session::startImport(bool asked) -> void
{
    ImportOptions options{
        .captures = host_.captain / "captures",
        .uploaded = host_.captain / "captures-uploaded",
        .state    = host_.captures,
        .server   = server_,
        .config   = host_.config,
        .mutex    = std::format("Local\\Memoria-import-{:x}", std::hash<std::wstring>{}((host_.captain / "captures").wstring())),
    };
    options.say = [this](std::string text)
    {
        post(std::move(text));
    };
    importer_ = std::make_unique<Importer>(std::move(options), std::make_unique<WinHttp>());
    importer_->start(asked);
}

// Asks for a code, then polls until the player approves it on the link page.
auto Session::link() -> void
{
    linker_ = std::jthread([this](std::stop_token stop)
                           {
                               WinHttp http;
                               // Replacing or destroying linker_ joins this thread on the game thread: end a request in flight at once.
                               const std::stop_callback cancel(stop, [&http]
                                                               {
                                                                   http.cancel();
                                                               });
                               const auto               code = startLink(http, server_);
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
                               // Only Mogsafe's own link page: never a URL the server could swap for another.
                               if (code->url.starts_with(server_ + "/link"))
                               {
                                   browse(code->url);
                               }
                               std::mutex                  mutex;
                               std::condition_variable_any wake;
                               const auto                  until = std::chrono::steady_clock::now() + code->expires;
                               while (std::chrono::steady_clock::now() < until)
                               {
                                   std::unique_lock lock(mutex);
                                   if (wake.wait_for(lock, stop, code->interval, []
                                                     {
                                                         return false;
                                                     }) ||
                                       stop.stop_requested())
                                   {
                                       return;
                                   }
                                   const auto result = pollLink(http, server_, code->poll);
                                   if (stop.stop_requested())
                                   {
                                       return;
                                   }
                                   if (!result)
                                   {
                                       post(paint(Color::Red, "Not linked: " + result.error() + ".") + " Run " + paint(Color::Cyan, "/memoria link") + " again.");
                                       return;
                                   }
                                   if (*result)
                                   {
                                       if (!saveLink(host_.config / "token", **result))
                                       {
                                           post(paint(Color::Red, "Not linked: cannot save the link in " + host_.config.string() + ".") + " Free some space, then run " + paint(Color::Cyan, "/memoria link") + " again.");
                                           return;
                                       }
                                       std::lock_guard inbox(inboxMutex_);
                                       arrived_ = **result;
                                       return;
                                   }
                               }
                               post(paint(Color::Red, "The code expired.") + " Run " + paint(Color::Cyan, "/memoria link") + " again.");
                           });
}

auto Session::linked() const -> std::optional<Linked>
{
    return loadLink(host_.config / "token");
}

auto Session::unlink(const std::string& why) -> void
{
    stopLive();
    say(paint(Color::Yellow, why + ".") + " Your captures stay on this PC until you run " + paint(Color::Cyan, "/memoria link") + ".");
}

auto Session::hello() -> std::string
{
    return liveHello(character_.name, character_.world, client::lobby::version(), CAPTURE_AGENT);
}

// A linked install saves chat when its account chose to. The request runs on its own thread; while one is in flight,
// another waits for the next call.
auto Session::refreshChat() -> void
{
    if (fetchingChat_.exchange(true))
    {
        return;
    }
    chatFetch_ = std::jthread([this](const std::stop_token& stop)
                              {
                                  WinHttp            http;
                                  std::stop_callback cancel(stop, [&]
                                                            {
                                                                http.cancel();
                                                            });
                                  if (const auto link = linked(); !link) // read here: the caller may be a packet hook
                                  {
                                      saveChat_ = false;
                                  }
                                  else if (const auto saved = fetchSaveChat(http, server_, link->token))
                                  {
                                      saveChat_ = *saved;
                                  }
                                  fetchingChat_ = false;
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

} // namespace memoria
