#include "core/recorder.h"

#include <picosha2.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <fstream>
#include <random>
#include <span>

namespace memoria
{
namespace
{

// Per character, since clients can share an install. It says whether the character's last capture was left open by a crash, so the next one can name it.
auto stateFile(const std::filesystem::path& dir, const xicap::CharacterIdentity& character) -> std::filesystem::path
{
    return dir / std::format("state-{}.txt", character.characterId);
}

constexpr auto REOPEN_EVERY = std::chrono::minutes(1);

auto parseHex(std::string_view text) -> std::optional<xicap::CaptureId>
{
    xicap::CaptureId id{};
    if (text.size() != id.size() * 2)
    {
        return std::nullopt;
    }
    for (size_t i = 0; i < id.size(); ++i)
    {
        const auto* first = text.data() + i * 2;
        if (std::from_chars(first, first + 2, id[i], 16).ptr != first + 2)
        {
            return std::nullopt;
        }
    }
    return id;
}

auto randomCaptureId() -> xicap::CaptureId
{
    std::random_device random;
    xicap::CaptureId   id{};
    std::ranges::generate(id, [&]
                          {
                              return static_cast<uint8_t>(random());
                          });
    return id;
}

auto sameCharacter(const xicap::CharacterIdentity& a, const xicap::CharacterIdentity& b) -> bool
{
    return a.contentId == b.contentId && a.characterId == b.characterId && a.worldId == b.worldId && a.characterName == b.characterName;
}

auto fileName(uint64_t timestamp, const xicap::CharacterIdentity& character, const xicap::CaptureId& id) -> std::string
{
    const std::chrono::sys_time<std::chrono::milliseconds> when{ std::chrono::milliseconds{ timestamp } };

    std::string name(character.characterName.data(), strnlen(character.characterName.data(), character.characterName.size()));
    std::ranges::replace_if(name, [](unsigned char c)
                            {
                                return !std::isalnum(c);
                            },
                            '_');
    return std::format("{:%Y%m%d-%H%M%S}-{}-{}.xicap", std::chrono::floor<std::chrono::seconds>(when), name.empty() ? "unknown" : name, picosha2::bytes_to_hex_string(id).substr(0, 8));
}

} // namespace

Recorder::Recorder(RecorderOptions options)
: options_(std::move(options))
, queue_(options_.queueBytes)
, lastFlush_(std::chrono::steady_clock::now())
{
    std::filesystem::create_directories(options_.dir);
    thread_ = std::thread(&Recorder::run, this);
}

Recorder::~Recorder()
{
    stop();
}

auto Recorder::packet(Event event) -> void
{
    event.kind = Event::Kind::Packet;
    queue_.push(std::move(event));
}

auto Recorder::login(uint64_t timestamp, Login who) -> void
{
    queue_.push({ .kind = Event::Kind::Login, .timestamp = timestamp, .login = std::move(who) });
}

auto Recorder::logout(uint64_t timestamp) -> void
{
    queue_.push({ .kind = Event::Kind::Logout, .timestamp = timestamp });
}

auto Recorder::note(uint64_t timestamp, std::string text) -> void
{
    queue_.push({ .kind = Event::Kind::Note, .timestamp = timestamp, .text = std::move(text) });
}

auto Recorder::sessionStart(uint64_t timestamp, const std::filesystem::path& path) -> void
{
    const auto utf8 = path.u8string();
    queue_.push({ .kind = Event::Kind::SessionStart, .timestamp = timestamp, .text = { utf8.begin(), utf8.end() } });
}

auto Recorder::sessionStop(uint64_t timestamp) -> void
{
    queue_.push({ .kind = Event::Kind::SessionStop, .timestamp = timestamp });
}

auto Recorder::status() const -> RecorderStatus
{
    std::lock_guard lock(statusMutex_);
    return status_;
}

auto Recorder::stop() -> void
{
    {
        std::lock_guard lock(wakeMutex_);
        if (stopping_)
        {
            return;
        }
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

auto Recorder::run() -> void
{
    std::vector<Event> events;
    while (true)
    {
        bool stopping = false;
        {
            std::unique_lock lock(wakeMutex_);
            stopping = wake_.wait_for(lock, options_.drainEvery, [this]
                                      {
                                          return stopping_;
                                      });
        }

        queue_.drain(events);
        for (const auto& event : events)
        {
            handle(event);
        }
        if (const auto dropped = queue_.takeDropped(); dropped > 0)
        {
            {
                std::lock_guard lock(statusMutex_);
                status_.dropped += dropped;
            }
            handle({ .kind = Event::Kind::Note, .text = std::format("dropped {} packets: the writer fell behind", dropped) });
        }

        const auto now = std::chrono::steady_clock::now();
        if (writer_ && now - lastFlush_ >= options_.flushEvery)
        {
            try
            {
                writer_->flush();
            }
            catch (const std::exception& error)
            {
                broken(error);
            }
            lastFlush_ = now;
        }
        if (stopping)
        {
            close();
            return;
        }
    }
}

auto Recorder::handle(const Event& event) -> void
{
    try
    {
        switch (event.kind)
        {
            case Event::Kind::Login:
                if (writer_ && login_ && sameCharacter(login_->character, event.login->character))
                {
                    mark(xicap::MarkerKind::Login);
                }
                else
                {
                    open(event.timestamp, *event.login);
                }
                break;
            case Event::Kind::Logout:
                mark(xicap::MarkerKind::Logout);
                break;
            case Event::Kind::Note:
                mark(xicap::MarkerKind::Note, event.text);
                break;
            case Event::Kind::SessionStart:
                openSession(std::u8string(event.text.begin(), event.text.end()));
                break;
            case Event::Kind::SessionStop:
                closeSession();
                break;
            case Event::Kind::Packet:
                if (!writer_ && login_ && std::chrono::steady_clock::now() >= reopenAt_)
                {
                    auto again       = *login_; // the last capture broke on a write error
                    again.midSession = true;
                    open(event.timestamp, again);
                }
                if (writer_)
                {
                    write(event);
                }
                break;
        }
    }
    catch (const std::exception& error)
    {
        broken(error);
    }
}

auto Recorder::config(const Login& who, xicap::StartContext start, const xicap::CaptureId& previous) const -> xicap::WriterConfig
{
    xicap::WriterConfig config{};
    config.captureAgent      = options_.captureAgent;
    config.clientVersion     = who.clientVersion;
    config.character         = who.character;
    config.clockSource       = xicap::ClockSource::Live;
    config.origin            = xicap::Origin::Live;
    config.network           = who.network;
    config.startContext      = start;
    config.previousCaptureId = previous;
    config.signingSeed       = options_.seed;
    config.captureId         = randomCaptureId();
    return config;
}

auto Recorder::open(uint64_t timestamp, const Login& who) -> void
{
    close();
    if (previous_ == xicap::CaptureId{})
    {
        std::ifstream state(stateFile(options_.dir, who.character));
        std::string   id;
        std::string   open;
        if (state >> id >> open && open == "open")
        {
            previous_ = parseHex(id).value_or(xicap::CaptureId{});
        }
    }
    auto       cfg  = config(who, who.midSession ? xicap::StartContext::MidSession : xicap::StartContext::Login, previous_);
    const auto path = options_.dir / fileName(timestamp, who.character, cfg.captureId);
    login_          = who;
    reopenAt_       = std::chrono::steady_clock::now() + REOPEN_EVERY;
    writer_         = std::make_unique<xicap::Writer>(path, std::move(cfg));
    previous_       = writer_->captureId();
    saveState(true);
    if (!who.midSession)
    {
        mark(xicap::MarkerKind::Login);
    }
    std::lock_guard lock(statusMutex_);
    status_.capture   = path;
    status_.publicKey = writer_->publicKey();
}

auto Recorder::write(const Event& event) -> void
{
    const xicap::OutPacket packet{
        .timestamp  = event.timestamp,
        .direction  = event.clientToServer ? xicap::PacketDirection::ClientToServer : xicap::PacketDirection::ServerToClient,
        .data       = std::as_bytes(std::span{ event.data }),
        .channel    = event.channel,
        .opcode     = event.opcode,
        .provenance = event.provenance,
    };
    if (session_)
    {
        try
        {
            session_->write(packet);
        }
        catch (const std::exception& error) // the main capture carries on
        {
            fail(error);
            session_.reset();
        }
    }
    writer_->write(packet);
    std::lock_guard lock(statusMutex_);
    status_.packets++;
}

auto Recorder::mark(xicap::MarkerKind kind, std::string_view text) -> void
{
    if (session_)
    {
        session_->mark(kind, text);
    }
    if (writer_)
    {
        writer_->mark(kind, text);
    }
}

auto Recorder::openSession(const std::filesystem::path& path) -> void
{
    closeSession();
    if (!writer_)
    {
        fail(std::runtime_error("no capture is open, so no session either"));
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    session_ = std::make_unique<xicap::Writer>(path, config(*login_, xicap::StartContext::MidSession, writer_->captureId()));
    std::lock_guard lock(statusMutex_);
    status_.session = path;
}

auto Recorder::closeSession() -> void
{
    if (!session_)
    {
        return;
    }
    try
    {
        session_->close();
    }
    catch (const std::exception& error)
    {
        fail(error);
    }
    session_.reset();
    std::lock_guard lock(statusMutex_);
    status_.session.clear();
}

auto Recorder::close() -> void
{
    closeSession();
    if (!writer_)
    {
        return;
    }
    try
    {
        writer_->close();
    }
    catch (const std::exception& error)
    {
        fail(error);
    }
    saveState(false);
    writer_.reset();
    login_.reset();
    std::lock_guard lock(statusMutex_);
    status_.capture.clear();
}

auto Recorder::fail(const std::exception& error) -> void
{
    std::lock_guard lock(statusMutex_);
    status_.error = error.what();
}

// A writer that failed once stays failed. Dropping it leaves the capture marked open, as a crash would, and the next packet starts a new one.
auto Recorder::broken(const std::exception& error) -> void
{
    fail(error);
    session_.reset();
    writer_.reset();
    std::lock_guard lock(statusMutex_);
    status_.capture.clear();
    status_.session.clear();
}

auto Recorder::saveState(bool open) -> void
{
    if (login_)
    {
        std::ofstream(stateFile(options_.dir, login_->character), std::ios::trunc) << picosha2::bytes_to_hex_string(previous_) << ' ' << (open ? "open" : "closed") << '\n';
    }
}

} // namespace memoria
