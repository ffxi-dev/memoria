#include "plugin/session.h"

#include "core/seed_file.h"
#include "plugin/client.h"
#include "plugin/winhttp.h"

#include <picosha2.h>
#include <xicap/constants.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace memoria
{
namespace
{

// Square Enix's retail zone servers, 124.150.152.0/21.
constexpr uint32_t RETAIL_FIRST = 0x7C969800;
constexpr uint32_t RETAIL_LAST  = 0x7C969FFF;

// GP_SERV_COMMAND_LOGOUT's LogoutState. Zone changes, the Mog House, and a cancelled logout keep the character in game.
auto leavesGame(uint8_t logoutState) -> bool
{
    constexpr uint8_t ZONECHANGE = 2;
    constexpr uint8_t MYROOM     = 3;
    constexpr uint8_t CANCEL     = 4;
    return logoutState != 0 && logoutState != ZONECHANGE && logoutState != MYROOM && logoutState != CANCEL;
}

// The zone server's IPv4 address in host order (credits: atom0s, via captain).
auto zoneServer() -> uint32_t
{
    static const uintptr_t sig = client::find("8B0D????????8D04808B8481????????C3");
    if (sig == 0)
    {
        return 0;
    }
    try
    {
        const auto ptr = *reinterpret_cast<const uint32_t*>(sig + 2);
        const auto ret = *reinterpret_cast<const uint32_t*>(ptr);
        return std::byteswap(*reinterpret_cast<const uint32_t*>(ret));
    }
    catch (...) // /EHa: a bad read is an access violation
    {
        return 0;
    }
}

auto characterOf(const uint8_t* data, uint32_t size) -> xicap::CharacterIdentity
{
    xicap::CharacterIdentity c{};
    if (size >= xicap::FFXIPacket::LOGIN_NAME_OFFSET + xicap::FFXIPacket::LOGIN_NAME_SIZE)
    {
        std::memcpy(&c.characterId, data + xicap::FFXIPacket::LOGIN_UNIQUE_NO_OFFSET, sizeof(c.characterId));
        const auto* name = reinterpret_cast<const char*>(data + xicap::FFXIPacket::LOGIN_NAME_OFFSET);
        std::memcpy(c.characterName.data(), name, strnlen(name, c.characterName.size() - 1));
    }
    return c;
}

// "key = value" lines; # and ; start comments.
auto readIni(const std::filesystem::path& path) -> std::optional<std::map<std::string, std::string>>
{
    std::ifstream in(path);
    if (!in)
    {
        return std::nullopt;
    }
    const auto trim = [](std::string_view s)
    {
        const auto first = s.find_first_not_of(" \t\r");
        return first == std::string_view::npos ? std::string{} : std::string(s.substr(first, s.find_last_not_of(" \t\r") - first + 1));
    };
    std::map<std::string, std::string> values;
    for (std::string line; std::getline(in, line);)
    {
        const auto eq = line.find('=');
        if (eq != std::string::npos && !line.starts_with('#') && !line.starts_with(';'))
        {
            values[trim(std::string_view(line).substr(0, eq))] = trim(std::string_view(line).substr(eq + 1));
        }
    }
    return values;
}

Session* instance_ = nullptr; // for the search hooks, which take a plain function

auto count(uint64_t n) -> std::string
{
    auto text = std::to_string(n);
    for (auto at = static_cast<std::ptrdiff_t>(text.size()) - 3; at > 0; at -= 3)
    {
        text.insert(static_cast<size_t>(at), ",");
    }
    return text;
}

auto duration(uint64_t seconds) -> std::string
{
    if (seconds < 60)
    {
        return std::format("{} s", seconds);
    }
    if (seconds < 3600)
    {
        return std::format("{} min", seconds / 60);
    }
    return std::format("{} h {} min", seconds / 3600, seconds / 60 % 60);
}

} // namespace

Session::Session(Host host)
: host_(std::move(host))
, config_(host_.config)
, captures_(host_.captures)
, captain_(host_.captain)
{
}

auto Session::start(std::optional<LateLogin> late) -> void
{
    const auto seed = loadOrCreateSeed(config_ / "key");
    if (!seed)
    {
        startError_ = seed.error();
        say("not recording: " + startError_);
        return; // stay loaded so /memoria status can say why
    }
    seed_ = *seed;
    if (auto config = readIni(config_ / "upload.ini"); config && (*config)["server"].starts_with("https://"))
    {
        server_ = (*config)["server"];
        while (server_.ends_with('/'))
        {
            server_.pop_back();
        }
    }

    try
    {
        startUploads();
        recorder_ = std::make_unique<Recorder>(RecorderOptions{
            .dir          = captures_,
            .seed         = seed_,
            .captureAgent = "Memoria " MEMORIA_VERSION_TEXT,
            .ended        = [this](const std::filesystem::path& capture)
            {
                if (!uploader_->ended(capture.filename().string()))
                {
                    return false;
                }
                post("The server ended the last recording; continuing in a new one.");
                return true;
            },
        });
        if (Importer::pending(captures_) && linked())
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
    instance_    = this;
    searchError_ = client::search::install([](bool clientToServer, const uint8_t* data, uint32_t size)
                                           {
                                               instance_->recordSearch(clientToServer, data, size);
                                           });

    // Loaded while already in a zone: record from here, marked mid-session.
    if (late)
    {
        Login login{ .network = network(), .midSession = true, .clientVersion = client::lobby::version() };
        login.character.characterId = late->characterId;
        std::memcpy(login.character.characterName.data(), late->name.data(), (std::min)(late->name.size(), login.character.characterName.size() - 1));
        characterError_ = client::lobby::character(login.character);
        character_      = login.character;
        loggedIn_       = true;
        loggedInAs_     = login.character.characterId;
        recorder_->login(clock_.nowMs(), std::move(login));
    }
    host_.log(std::format("Memoria {} recording to {}", MEMORIA_VERSION_TEXT, captures_.string()));
}

Session::~Session()
{
    linker_   = {}; // stops and joins
    reminder_ = {};
    importer_.reset(); // cancels its request and joins
    stopLive();
    closing_.clear(); // joins
    client::search::remove();
    if (recorder_)
    {
        recorder_->stop();
        recorder_.reset();
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
    if (id == xicap::FFXIPacket::LOGIN_ZONE)
    {
        auto who = characterOf(data, size);
        if (!loggedIn_ || who.characterId != loggedInAs_)
        {
            characterError_ = client::lobby::character(who);
            character_      = who;
            loggedIn_       = true;
            loggedInAs_     = who.characterId;
            remind();
            offerImport();
            recorder_->login(clock_.nowMs(), { .character = who, .network = network(), .clientVersion = client::lobby::version() });
            if (live_)
            {
                live_->setHello(hello());
            }
        }
    }
    record(false, size, data, modified, sizeChunk, dataChunk, injected, blocked);
    if (id == xicap::FFXIPacket::LOGOUT && size > 4 && leavesGame(data[4]))
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
        if (!importer_ && Importer::pending(captures_))
        {
            startImport(false);
        }
        offerImport();
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

auto Session::network() -> xicap::Network
{
    const auto ip = zoneServer();
    if (ip == 0)
    {
        return xicap::Network::Unknown;
    }
    return ip >= RETAIL_FIRST && ip <= RETAIL_LAST ? xicap::Network::Retail : xicap::Network::Private;
}

// Runs until the plugin unloads; the token and paused files decide each pass whether it sends.
auto Session::startUploads() -> void
{
    UploadOptions options{
        .dir    = captures_,
        .server = server_,
        .config = config_,
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
    uploader_ = std::make_unique<Uploader>(std::move(options), std::make_unique<WinHttp>("Memoria/" MEMORIA_VERSION_TEXT));
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

// Once a day while unlinked: the date of the last reminder is kept across restarts.
auto Session::remind() -> void
{
    const auto  path  = config_ / "reminded";
    const auto  today = std::format("{:%F}", std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
    std::string last;
    std::getline(std::ifstream(path), last);
    if (linked() || last == today)
    {
        return;
    }
    std::ofstream(path, std::ios::trunc) << today;
    say(paint(Color::Yellow, "Your captures stay on this PC.") + " Run " + paint(Color::Cyan, "/memoria link") + " to share them with Vault.");
}

// Once per install, ever: while linked and captain captures wait, at login or link.
auto Session::offerImport() -> void
{
    std::error_code ec;
    if (reminding_ || !linked() || std::filesystem::exists(config_ / "import-reminded", ec))
    {
        return;
    }
    reminding_ = true;
    reminder_  = std::jthread([this]
                              {
                                 // Clients logging in together race to create the reminded file; only the winner speaks.
                                 if (remindImport(config_, captain_ / "captures"))
                                 {
                                     post("Run " + paint(Color::Cyan, "/memoria import") + " to upload existing Captain captures.");
                                 }
                                 reminding_ = false;
                              });
}

// Clients sharing an install import through the one whose importer holds the named mutex.
auto Session::startImport(bool asked) -> void
{
    ImportOptions options{
        .captures = captain_ / "captures",
        .uploaded = captain_ / "captures-uploaded",
        .state    = captures_,
        .server   = server_,
        .config   = config_,
        .mutex    = std::format("Local\\Memoria-import-{:x}", std::hash<std::wstring>{}((captain_ / "captures").wstring())),
    };
    options.say = [this](std::string text)
    {
        post(std::move(text));
    };
    importer_ = std::make_unique<Importer>(std::move(options), std::make_unique<WinHttp>("Memoria/" MEMORIA_VERSION_TEXT));
    importer_->start(asked);
}

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

// Asks for a code, then polls until the player approves it on the link page.
auto Session::link() -> void
{
    linker_ = std::jthread([this](std::stop_token stop)
                           {
                               WinHttp    http("Memoria/" MEMORIA_VERSION_TEXT);
                               const auto now  = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                               const auto code = startLink(http, server_, seed_, static_cast<uint64_t>(now));
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
                                   if (!result)
                                   {
                                       post(paint(Color::Red, "Not linked: " + result.error() + ".") + " Run " + paint(Color::Cyan, "/memoria link") + " again.");
                                       return;
                                   }
                                   if (*result)
                                   {
                                       saveLink(config_ / "token", **result);
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
    return loadLink(config_ / "token");
}

auto Session::unlink(const std::string& why) -> void
{
    stopLive();
    say(paint(Color::Yellow, why + ".") + " Your captures stay on this PC until you run " + paint(Color::Cyan, "/memoria link") + ".");
}

auto Session::hello() -> std::string
{
    return liveHello(character_.characterName.data(), character_.worldName.data(), client::lobby::version(), "Memoria " MEMORIA_VERSION_TEXT);
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

auto Session::status() -> void
{
    say(paint(Color::Grey, "Memoria " MEMORIA_VERSION_TEXT));
    const auto s = recorder_->status();
    if (s.capture.empty())
    {
        say(paint(Color::Grey, "Waiting for a login."));
    }
    else
    {
        std::error_code   ec;
        const auto        onDisk  = std::filesystem::file_size(s.capture, ec);
        const auto        raw     = s.bytesIn + s.bytesOut;
        const auto        seconds = (std::max)(uint64_t{ 1 }, (s.lastMs - s.firstMs) / 1000);
        const std::string name(character_.characterName.data());
        const std::string world(character_.worldName.data());
        say(std::format("{} {}{} for {} · {} packets ({:.1f}/s) · {}{}", paint(Color::Green, "Recording"), name, world.empty() ? "" : " on " + world, duration(seconds), count(s.packets), static_cast<double>(s.packets) / static_cast<double>(seconds), sizeText(onDisk), onDisk > 0 ? std::format(" ({:.1f}x smaller)", static_cast<double>(raw) / static_cast<double>(onDisk)) : ""));
    }
    const auto version = client::lobby::version();
    auto       client  = "Client " + (version.empty() ? paint(Color::Grey, "unknown before the first login") : version);
    if (searchError_.empty())
    {
        client += std::format(" · {} search packets", count(s.search));
    }
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
    if (!link || std::filesystem::exists(config_ / "paused"))
    {
        say("Uploads: " + paint(Color::Yellow, link ? "paused: /memoria upload on" : "not linked: /memoria link"));
        return;
    }
    // "All sent" means Mogsafe holds every byte recorded so far, whichever client sharing this folder sends it.
    const auto  files  = uploader_->status();
    uint64_t    behind = 0;
    uint64_t    kept   = 0; // in recordings Mogsafe cut off: on this PC only
    int         cut    = 0;
    const auto* failed = static_cast<const UploadStatus*>(nullptr);
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
    for (const auto& u : files | std::views::filter(&UploadStatus::refused))
    {
        say(paint(Color::Red, std::format("Mogsafe refused {}: {}", u.name, u.error)));
    }
}

auto Session::command(const std::vector<std::string>& args, std::string_view line) -> void
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
    else if (sub == "key")
    {
        const auto s = recorder_->status();
        say(s.capture.empty() ? "the key shows once a capture opens" : "public key " + picosha2::bytes_to_hex_string(s.publicKey));
    }
    else if (sub == "note" && args.size() > 2)
    {
        recorder_->note(clock_.nowMs(), args | std::views::drop(2) | std::views::join_with(' ') | std::ranges::to<std::string>());
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
        recorder_->sessionStart(clock_.nowMs(), std::u8string(path.begin(), path.end()));
        say(std::format("session to {}", path));
    }
    else if (sub == "session" && args.size() > 2 && args[2] == "stop")
    {
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
            createNew(config_ / "import-reminded");
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
    else if (sub == "live" && live_)
    {
        live_->reopen();
        say("Opening the live link again.");
    }
    else if (sub == "live")
    {
        live_ = std::make_unique<Live>(server_, link->token, recorder_->live(), hello(), [this](std::string text)
                                       {
                                           post(std::move(text));
                                       });
        say(paint(Color::Grey, "Asking Vault for a live link..."));
    }
    else if (sub == "unlink" && linked())
    {
        std::error_code ec;
        std::filesystem::remove(config_ / "token", ec);
        unlink("Unlinked");
    }
    else if (sub == "upload" && args.size() > 2 && (args[2] == "off" || args[2] == "on"))
    {
        const auto      paused = config_ / "paused";
        std::error_code ec;
        if (args[2] == "off")
        {
            std::ofstream{ paused };
            stopLive();
            say(paint(Color::Yellow, "Uploads paused.") + " " + paint(Color::Cyan, "/memoria upload on") + " resumes them.");
        }
        else
        {
            std::filesystem::remove(paused, ec);
            say(linked() ? paint(Color::Green, "Uploads on.") : paint(Color::Yellow, "Uploads off: not linked: /memoria link"));
        }
    }
    else
    {
        say("/memoria status | link | unlink | live [off] | upload on|off | import | key | note <text> | session start <file.xicap> | session stop");
    }
}

} // namespace memoria
