#include "plugin/memoria.h"

#include "core/seed_file.h"
#include "plugin/client.h"
#include "plugin/pol.h"
#include "plugin/winhttp.h"

#include <Commands.h>
#include <Memory.h>
#include <xicap/constants.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace memoria
{
namespace
{

constexpr uint16_t ZONE_IN = 0x00A; // GP_SERV_COMMAND_LOGIN
constexpr uint16_t LOGOUT  = 0x00B; // GP_SERV_COMMAND_LOGOUT

// GP_SERV_COMMAND_LOGOUT's LogoutState: zone changes and a cancelled logout
// keep the character in game; every other state leaves it.
auto leavesGame(uint8_t logoutState) -> bool
{
    constexpr uint8_t ZONECHANGE = 2;
    constexpr uint8_t MYROOM     = 3;
    constexpr uint8_t CANCEL     = 4;
    return logoutState != 0 && logoutState != ZONECHANGE && logoutState != MYROOM && logoutState != CANCEL;
}

// Square Enix's retail zone servers, 124.150.152.0/21.
constexpr uint32_t RETAIL_FIRST = 0x7C969800;
constexpr uint32_t RETAIL_LAST  = 0x7C969FFF;

// The client build, as the client prints it (credits: atom0s).
auto clientBuild() -> std::string
{
    try
    {
        const auto sig = client::find("68????????E8????????83C4046AFF");
        if (sig == 0)
        {
            return {};
        }
        const auto* text = *reinterpret_cast<const char* const*>(sig + 1);
        // The client prints "[[ [  [  Version 19:08:56 Oct  5 2026  ]  ] ]]":
        // keep the words, "Version 19:08:56 Oct 5 2026", which fit the header.
        std::string build;
        for (const char c : std::string_view(text, strnlen(text, 48)))
        {
            if (c == '[' || c == ']' || std::isspace(static_cast<unsigned char>(c)))
            {
                if (!build.empty() && build.back() != ' ')
                {
                    build += ' ';
                }
                continue;
            }
            build += c;
        }
        while (!build.empty() && build.back() == ' ')
        {
            build.pop_back();
        }
        return build;
    }
    catch (...) // /EHa: a bad read is an access violation
    {
        return {};
    }
}

// The zone server's IPv4 address, host order (credits: atom0s, via captain).
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
        const auto le  = *reinterpret_cast<const uint32_t*>(ret);
        return _byteswap_ulong(le);
    }
    catch (...)
    {
        return 0;
    }
}

// The character a zone-in names: UniqueNo at byte 4, the name at byte 132.
auto characterOf(const uint8_t* data, uint32_t size) -> xicap::CharacterIdentity
{
    xicap::CharacterIdentity c{};
    if (size >= xicap::FFXIPacket::LOGIN_NAME_OFFSET + xicap::FFXIPacket::LOGIN_NAME_SIZE)
    {
        std::memcpy(&c.characterId, data + xicap::FFXIPacket::LOGIN_UNIQUE_NO_OFFSET, sizeof(c.characterId));
        const auto* name = reinterpret_cast<const char*>(data + xicap::FFXIPacket::LOGIN_NAME_OFFSET);
        std::memcpy(c.characterName.data(), name, strnlen(name, c.characterName.size() - 1)); // nothing after the NUL
    }
    return c;
}

// The plugin the client hooks report to; Ashita makes one.
Plugin* instance_ = nullptr;

auto hex(const xicap::PublicKey& key) -> std::string
{
    std::string out;
    for (const auto b : key)
    {
        out += std::format("{:02x}", b);
    }
    return out;
}

} // namespace

auto Plugin::GetName() const -> const char*
{
    return "Memoria";
}

auto Plugin::GetAuthor() const -> const char*
{
    return "sruon";
}

auto Plugin::GetDescription() const -> const char*
{
    return "Records every packet of a retail session to xicap and uploads it to Vault.";
}

auto Plugin::GetLink() const -> const char*
{
    return "https://github.com/ffxi-dev/memoria";
}

auto Plugin::GetVersion() const -> double
{
    return MEMORIA_VERSION; // from CMakeLists.txt
}

auto Plugin::GetPriority() const -> int32_t
{
    // Higher runs later: after every other plugin, so the blocked and modified
    // flags reflect them all.
    return INT32_MAX; // not numeric_limits: windows.h defines max
}

auto Plugin::GetFlags() const -> uint32_t
{
    return static_cast<uint32_t>(Ashita::PluginFlags::UsePackets) | static_cast<uint32_t>(Ashita::PluginFlags::UseCommands) |
           static_cast<uint32_t>(Ashita::PluginFlags::UseDirect3D); // a frame callback for the search hooks
}

auto Plugin::Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t id) -> bool
{
    UNREFERENCED_PARAMETER(id);
    core_ = core;
    log_  = logger;

    const std::filesystem::path install = core_->GetInstallPath();
    polModule_                          = (install / "polplugins" / "Memoria.dll").wstring();
    const auto seed                     = loadOrCreateSeed(install / "config" / "memoria" / "key");
    if (!seed)
    {
        startError_ = seed.error();
        say("not recording: " + startError_);
        return true; // stay loaded so /memoria status can say why
    }

    RecorderOptions options;
    options.dir           = install / "captures" / "memoria";
    options.seed          = *seed;
    options.captureAgent  = "Memoria " MEMORIA_VERSION_TEXT;
    options.clientVersion = clientBuild();
    try
    {
        recorder_ = std::make_unique<Recorder>(std::move(options));
        startUploads(install);
        lobbyError_  = client::lobby::install();
        instance_    = this;
        searchError_ = client::search::install([](bool clientToServer, const uint8_t* data, uint32_t size)
                                               {
                                                   instance_->recordSearch(clientToServer, data, size);
                                               });
    }
    catch (const std::exception& error) // the captures folder cannot be made, say
    {
        startError_ = error.what();
        say("not recording: " + startError_);
        return true;
    }

    // Loaded while already in a zone: record from here, marked mid-session.
    auto* party = core_->GetMemoryManager()->GetParty();
    if (core_->GetMemoryManager()->GetPlayer()->GetLoginStatus() == 2 && party->GetMemberServerId(0) != 0)
    {
        Login late{};
        late.character.characterId = party->GetMemberServerId(0);
        const char* name           = party->GetMemberName(0);
        std::memcpy(late.character.characterName.data(), name, strnlen(name, late.character.characterName.size() - 1));
        late.network    = network();
        late.midSession = true;
        recorder_->login(clock_.nowMs(), late);
        loggedIn_   = true;
        loggedInAs_ = late.character.characterId;
    }
    log_->Logf(static_cast<uint32_t>(Ashita::LogLevel::Info), "Memoria", "Memoria %.1f recording to %s", GetVersion(), (install / "captures" / "memoria").string().c_str());
    return true;
}

auto Plugin::Release() -> void
{
    client::search::remove(); // before the recorder it feeds goes
    client::lobby::remove();
    if (recorder_)
    {
        recorder_->stop();
        recorder_.reset();
    }
    uploader_.reset(); // waits for the request in flight, up to WinHTTP's timeouts
    if (uploadMutex_)
    {
        ::CloseHandle(uploadMutex_);
        uploadMutex_ = nullptr;
    }
}

auto Plugin::HandleIncomingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> bool
{
    if (!recorder_)
    {
        return false;
    }
    // A zone-in after character select is a login; its marker goes before it.
    // So is a zone-in naming another character: a disconnect sends the client
    // back to the title screen without a logout.
    if (id == ZONE_IN)
    {
        const auto who = characterOf(data, size);
        if (!loggedIn_ || who.characterId != loggedInAs_)
        {
            Login      login{ .character = who, .network = network(), .midSession = false };
            const auto version = client::lobby::version();
            std::memcpy(login.clientVersion.data(), version.data(), (std::min)(version.size(), login.clientVersion.size() - 1));
            recorder_->login(clock_.nowMs(), login);
            loggedIn_   = true;
            loggedInAs_ = who.characterId;
        }
    }
    record(false, id, size, data, modified, sizeChunk, dataChunk, injected, blocked);
    if (id == LOGOUT && size > 4 && leavesGame(data[4]))
    {
        recorder_->logout(clock_.nowMs());
        loggedIn_ = false;
    }
    return false;
}

auto Plugin::HandleOutgoingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> bool
{
    if (recorder_)
    {
        record(true, id, size, data, modified, sizeChunk, dataChunk, injected, blocked);
    }
    return false;
}

auto Plugin::record(bool clientToServer, uint16_t id, uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void
{
    UNREFERENCED_PARAMETER(id);
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
    // Ashita hands each packet as a copy, but the chunk it came in as is: a
    // packet whose chunk differs from the last one's (pointer, size, or first
    // bytes, which carry the chunk's sequence number) is the first of its chunk.
    if (dataChunk && sizeChunk > 0)
    {
        auto&    last = lastChunk_[clientToServer ? 1 : 0];
        ChunkKey key{ reinterpret_cast<uintptr_t>(dataChunk), sizeChunk, {} };
        std::memcpy(key.head.data(), dataChunk, (std::min)(sizeChunk, static_cast<uint32_t>(key.head.size())));
        if (!last || last->pointer != key.pointer || last->size != key.size || last->head != key.head)
        {
            provenance |= xicap::PacketFlag::FirstInChunk;
        }
        last = key;
    }
    recorder_->packet(Event{
        .kind           = Event::Kind::Packet,
        .timestamp      = clock_.nowMs(),
        .clientToServer = clientToServer,
        .provenance     = provenance,
        .data           = { data, data + size },
    });
}

// Search channel: the plaintext request or reply, from its 4-byte size; the
// opcode is byte 0x0B, the request type LSB's search server switches on.
auto Plugin::recordSearch(bool clientToServer, const uint8_t* data, uint32_t size) -> void
{
    recorder_->packet(Event{
        .kind           = Event::Kind::Packet,
        .timestamp      = clock_.nowMs(),
        .clientToServer = clientToServer,
        .channel        = static_cast<uint8_t>(xicap::Channel::Search),
        .opcode         = data[0x0B],
        .data           = { data, data + size },
    });
}

// Ashita refuses a plugin that asks for Direct3D callbacks and does not
// accept the device here.
auto Plugin::Direct3DInitialize(IDirect3DDevice8* device) -> bool
{
    UNREFERENCED_PARAMETER(device);
    return true;
}

auto Plugin::Direct3DBeginScene(bool isRenderingBackBuffer) -> void
{
    UNREFERENCED_PARAMETER(isRenderingBackBuffer);
    client::search::tick();
    takePol();
}

// The POL plugin is this DLL loaded from polplugins/: usually a separate copy,
// with its own queue, found by its path; else this copy is the one.
auto Plugin::takePol() -> void
{
    if (!recorder_)
    {
        return;
    }
    auto take = &memoriaPolTake;
    if (const auto pol = ::GetModuleHandleW(polModule_.c_str()))
    {
        if (const auto found = ::GetProcAddress(pol, "memoriaPolTake"))
        {
            take      = reinterpret_cast<decltype(take)>(found);
            polFound_ = true;
        }
    }
    take([](void* context, const PolRecord* r)
         {
             static_cast<Plugin*>(context)->polTaken_++;
             static_cast<Plugin*>(context)->recorder_->packet(Event{
                 .kind           = Event::Kind::Packet,
                 .timestamp      = r->timestamp,
                 .clientToServer = r->clientToServer != 0,
                 .channel        = r->channel,
                 .opcode         = r->opcode,
                 .data           = { r->data, r->data + r->size },
             });
         },
         this);
}

auto Plugin::network() -> xicap::Network
{
    const auto ip = zoneServer();
    if (ip == 0)
    {
        return xicap::Network::Unknown;
    }
    return ip >= RETAIL_FIRST && ip <= RETAIL_LAST ? xicap::Network::Retail : xicap::Network::Private;
}

// config/memoria/upload.ini holds Mogsafe's address and the Access service
// token, as "key = value" lines: server, client_id, client_secret. Without it,
// Memoria records and keeps everything locally.
auto Plugin::startUploads(const std::filesystem::path& install) -> void
{
    std::ifstream                      in(install / "config" / "memoria" / "upload.ini");
    std::map<std::string, std::string> config;
    std::string                        line;
    if (!in)
    {
        uploadsOff_ = "no config/memoria/upload.ini";
        return;
    }
    while (std::getline(in, line))
    {
        const auto eq = line.find('=');
        if (eq == std::string::npos || line.starts_with('#') || line.starts_with(';'))
        {
            continue;
        }
        const auto trim = [](std::string s)
        {
            s.erase(0, s.find_first_not_of(" \t\r"));
            s.erase(s.find_last_not_of(" \t\r") + 1);
            return s;
        };
        config[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    if (!config["server"].starts_with("https://") || config["client_id"].empty() || config["client_secret"].empty())
    {
        uploadsOff_ = "upload.ini needs an https server, client_id, and client_secret";
        return;
    }
    UploadOptions options;
    options.dir         = install / "captures" / "memoria";
    options.server      = config["server"];
    options.credentials = { { "CF-Access-Client-Id", config["client_id"] }, { "CF-Access-Client-Secret", config["client_secret"] } };
    while (options.server.ends_with('/'))
    {
        options.server.pop_back();
    }
    // One uploader per captures folder, across every client sharing the install:
    // the one whose upload thread holds this mutex. When that client exits,
    // Windows lets the next one take it.
    const auto mutexName = std::format("Local\\Memoria-upload-{:x}", std::hash<std::wstring>{}(options.dir.wstring()));
    uploadMutex_         = ::CreateMutexA(nullptr, FALSE, mutexName.c_str());
    options.mayRun       = [mutex = uploadMutex_]
    {
        const auto r = ::WaitForSingleObject(mutex, 0);
        return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    };
    uploader_ = std::make_unique<Uploader>(std::move(options), std::make_unique<WinHttp>(std::format("Memoria/{:.1f}", GetVersion())));
    uploader_->start();
}

auto Plugin::say(const std::string& text) -> void
{
    core_->GetChatManager()->Writef(1, false, "\x1E\x51[Memoria]\x1E\x01 %s", text.c_str());
}

auto Plugin::HandleCommand(int32_t mode, const char* command, bool injected) -> bool
{
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(injected);
    std::vector<std::string> args;
    Ashita::Commands::GetCommandArgs(command, &args);
    if (args.empty() || args[0] != "/memoria")
    {
        return false;
    }
    const std::string sub = args.size() > 1 ? args[1] : "status";
    if (!recorder_)
    {
        say("not recording: " + startError_);
        return true;
    }
    const auto status = recorder_->status();
    if (sub == "status")
    {
        say(status.capture.empty() ? "waiting for a login" : "recording to " + status.capture.filename().string());
        if (!status.session.empty())
        {
            say("session to " + status.session.string());
        }
        say(std::format("{} packets, {} dropped", status.packets, status.dropped));
        if (!status.error.empty())
        {
            say("last write error: " + status.error);
        }
        say(polFound_ ? std::format("PlayOnline: {} records", polTaken_) : "PlayOnline: the POL plugin is not loaded");
        say(client::lobby::version().empty() ? "client version: unknown until character select" + (lobbyError_.empty() ? "" : " (" + lobbyError_ + ")")
                                             : "client version " + client::lobby::version());
        if (!searchError_.empty())
        {
            say("search not recorded: " + searchError_);
        }
        if (!uploader_)
        {
            say("uploads off: " + uploadsOff_);
        }
        else
        {
            for (const auto& u : uploader_->status())
            {
                if (u.closed && u.error.empty())
                {
                    continue; // done; the file stays in the captures folder
                }
                say(std::format("{}: {} of {} bytes uploaded{}", u.name, u.sent, u.size, u.error.empty() ? "" : " (" + u.error + ")"));
            }
        }
    }
    else if (sub == "key")
    {
        say(status.capture.empty() ? "the key shows once a capture opens" : "public key " + hex(status.publicKey));
    }
    else if (sub == "session" && args.size() > 2 && args[2] == "start")
    {
        // The path is the rest of the line as typed: it may hold spaces.
        std::string_view rest(command);
        const auto       at = rest.find("start");
        std::string      path(rest.substr(at + 5));
        path.erase(0, path.find_first_not_of(" \""));
        path.erase(path.find_last_not_of(" \"") + 1);
        if (path.empty())
        {
            say("/memoria session start <file.xicap>");
            return true;
        }
        recorder_->sessionStart(clock_.nowMs(), std::filesystem::path(std::u8string(path.begin(), path.end())));
        say("session to " + path);
    }
    else if (sub == "session" && args.size() > 2 && args[2] == "stop")
    {
        recorder_->sessionStop(clock_.nowMs());
        say("session closed");
    }
    else if (sub == "note" && args.size() > 2)
    {
        std::string text;
        for (size_t i = 2; i < args.size(); ++i)
        {
            text += (i > 2 ? " " : "") + args[i];
        }
        recorder_->note(clock_.nowMs(), text);
        say("noted");
    }
    else
    {
        say("/memoria status | key | note <text> | session start <file.xicap> | session stop");
    }
    return true;
}

} // namespace memoria

namespace
{
// What this copy of the DLL made: Ashita destroys both through one export.
memoria::Plugin*    game_ = nullptr;
memoria::PolPlugin* pol_  = nullptr;
} // namespace

extern "C" __declspec(dllexport) IPlugin* __stdcall expCreatePlugin(const char* args)
{
    UNREFERENCED_PARAMETER(args);
    game_ = new memoria::Plugin();
    return game_;
}

extern "C" __declspec(dllexport) IPolPlugin* __stdcall expCreatePolPlugin(const char* args)
{
    UNREFERENCED_PARAMETER(args);
    pol_ = new memoria::PolPlugin();
    return pol_;
}

extern "C" __declspec(dllexport) void __stdcall expDestroyPlugin(void* instance)
{
    if (instance == static_cast<IPlugin*>(game_))
    {
        delete game_;
        game_ = nullptr;
    }
    else if (instance == static_cast<IPolPlugin*>(pol_))
    {
        delete pol_;
        pol_ = nullptr;
    }
}

extern "C" __declspec(dllexport) double __stdcall expGetInterfaceVersion()
{
    return ASHITA_INTERFACE_VERSION;
}
