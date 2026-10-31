#include "core/recorder.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <random>
#include <span>
#include <string_view>

namespace memoria
{
namespace
{

// A state file per character remembers its last capture, so a capture started
// after a crash names the one the crash cut off. Per character because several
// clients can share one install, and a character plays in one at a time.
auto stateFile(const std::filesystem::path& dir, const xicap::CharacterIdentity& character) -> std::filesystem::path
{
    return dir / std::format("state-{}.txt", character.characterId);
}

// After a write error, the next packet opens a new capture, at most this often.
constexpr auto REOPEN_EVERY = std::chrono::minutes(1);

// The PlayOnline login comes before any character's: that much of it waits
// for the first capture.
constexpr size_t EARLY_BYTES = 4 * 1024 * 1024;

auto hex(const xicap::CaptureId& id) -> std::string
{
    std::string out;
    for (const auto b : id)
    {
        out += std::format("{:02x}", b);
    }
    return out;
}

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
    while (std::ranges::all_of(id, [](uint8_t b)
                               {
                                   return b == 0;
                               }))
    {
        for (auto& b : id)
        {
            b = static_cast<uint8_t>(random());
        }
    }
    return id;
}

auto sameCharacter(const xicap::CharacterIdentity& a, const xicap::CharacterIdentity& b) -> bool
{
    return a.contentId == b.contentId && a.characterId == b.characterId && a.worldId == b.worldId &&
           a.characterName == b.characterName;
}

// A file name a person can read: when, who, and which capture.
auto fileName(uint64_t timestamp, const xicap::CharacterIdentity& character, const xicap::CaptureId& id) -> std::string
{
    const auto  when = std::chrono::sys_time<std::chrono::milliseconds>{ std::chrono::milliseconds{ timestamp } };
    std::string name(character.characterName.data(), strnlen(character.characterName.data(), character.characterName.size()));
    std::ranges::replace_if(name, [](char c)
                            {
                                return !std::isalnum(static_cast<unsigned char>(c));
                            },
                            '_');
    if (name.empty())
    {
        name = "unknown";
    }
    return std::format("{:%Y%m%d-%H%M%S}-{}-{}.xicap", std::chrono::floor<std::chrono::seconds>(when), name, hex(id).substr(0, 8));
}

} // namespace

Recorder::Recorder(RecorderOptions options)
: options_(std::move(options))
, queue_(options_.queueBytes)
{
    std::filesystem::create_directories(options_.dir);
    lastFlush_ = std::chrono::steady_clock::now();
    thread_    = std::thread([this]
                             {
                              run();
                             });
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

auto Recorder::login(uint64_t timestamp, const Login& who) -> void
{
    Event event{ .kind = Event::Kind::Login, .timestamp = timestamp };
    event.data.resize(sizeof(Login));
    std::memcpy(event.data.data(), &who, sizeof(Login));
    queue_.push(std::move(event));
}

auto Recorder::logout(uint64_t timestamp) -> void
{
    queue_.push(Event{ .kind = Event::Kind::Logout, .timestamp = timestamp });
}

auto Recorder::note(uint64_t timestamp, std::string_view text) -> void
{
    queue_.push(Event{ .kind = Event::Kind::Note, .timestamp = timestamp, .data = { text.begin(), text.end() } });
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
            wake_.wait_for(lock, options_.drainEvery, [this]
                           {
                               return stopping_;
                           });
            stopping = stopping_;
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
            // The capture says where it has a gap.
            const auto text = std::format("dropped {} packets: the writer fell behind", dropped);
            handle(Event{ .kind = Event::Kind::Note, .data = { text.begin(), text.end() } });
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
            {
                Login who{};
                std::memcpy(&who, event.data.data(), std::min(event.data.size(), sizeof(Login)));
                if (writer_ && login_ && sameCharacter(login_->character, who.character))
                {
                    writer_->mark(xicap::MarkerKind::Login);
                }
                else
                {
                    open(event.timestamp, who);
                }
                break;
            }
            case Event::Kind::Logout:
                if (writer_)
                {
                    writer_->mark(xicap::MarkerKind::Logout);
                }
                break;
            case Event::Kind::Note:
                if (writer_)
                {
                    writer_->mark(xicap::MarkerKind::Note, { reinterpret_cast<const char*>(event.data.data()), event.data.size() });
                }
                break;
            case Event::Kind::Packet:
                if (!writer_ && login_ && std::chrono::steady_clock::now() >= reopenAt_)
                {
                    // The last capture broke on a write error: carry on in a new one.
                    auto again       = *login_;
                    again.midSession = true;
                    open(event.timestamp, again);
                }
                if (!writer_)
                {
                    // No login seen yet: nothing to name the capture after.
                    // PlayOnline traffic waits for the first one.
                    if (!login_ && event.channel != static_cast<uint8_t>(xicap::Channel::Game) && earlyBytes_ + event.data.size() <= EARLY_BYTES)
                    {
                        earlyBytes_ += event.data.size();
                        early_.push_back(event);
                    }
                    break;
                }
                write(event);
                break;
        }
    }
    catch (const std::exception& error)
    {
        broken(error);
    }
}

auto Recorder::open(uint64_t timestamp, const Login& who) -> void
{
    close();
    if (previous_ == xicap::CaptureId{})
    {
        // This character's capture left open by the last run was cut off by a
        // crash: this one continues it.
        std::ifstream state(stateFile(options_.dir, who.character));
        std::string   id;
        std::string   open;
        if (state >> id >> open && open == "open")
        {
            previous_ = parseHex(id).value_or(xicap::CaptureId{});
        }
    }
    xicap::WriterConfig config{};
    config.captureAgent      = options_.captureAgent;
    config.clientVersion     = who.clientVersion[0] != 0 ? std::string(who.clientVersion.data(), strnlen(who.clientVersion.data(), who.clientVersion.size())) : options_.clientVersion;
    config.character         = who.character;
    config.clockSource       = xicap::ClockSource::Live;
    config.origin            = xicap::Origin::Live;
    config.network           = who.network;
    config.startContext      = who.midSession ? xicap::StartContext::MidSession : xicap::StartContext::Login;
    config.previousCaptureId = previous_;
    config.signingSeed       = options_.seed;
    config.captureId         = randomCaptureId(); // known up front, so it can name the file

    const auto path = options_.dir / fileName(timestamp, who.character, config.captureId);
    login_          = who;
    reopenAt_       = std::chrono::steady_clock::now() + REOPEN_EVERY;
    writer_         = std::make_unique<xicap::Writer>(path, std::move(config));
    previous_       = writer_->captureId(); // the next capture of this run continues this one
    saveState(true);
    for (const auto& event : early_)
    {
        write(event); // before the login marker: it came first
    }
    early_.clear();
    earlyBytes_ = 0;
    if (!who.midSession)
    {
        writer_->mark(xicap::MarkerKind::Login);
    }
    std::lock_guard lock(statusMutex_);
    status_.capture   = path;
    status_.publicKey = writer_->publicKey();
}

auto Recorder::write(const Event& event) -> void
{
    writer_->write({
        .timestamp  = event.timestamp,
        .direction  = event.clientToServer ? xicap::PacketDirection::ClientToServer : xicap::PacketDirection::ServerToClient,
        .data       = std::as_bytes(std::span{ event.data }),
        .channel    = event.channel,
        .opcode     = event.opcode,
        .provenance = event.provenance,
    });
    std::lock_guard lock(statusMutex_);
    status_.packets++;
}

auto Recorder::close() -> void
{
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

// A writer that failed once stays failed: drop it, leaving the capture marked
// open as a crash would, and let the next packet start a new one.
auto Recorder::broken(const std::exception& error) -> void
{
    fail(error);
    writer_.reset();
    std::lock_guard lock(statusMutex_);
    status_.capture.clear();
}

auto Recorder::saveState(bool open) -> void
{
    if (!login_)
    {
        return;
    }
    std::ofstream state(stateFile(options_.dir, login_->character), std::ios::trunc);
    state << hex(previous_) << ' ' << (open ? "open" : "closed") << '\n';
}

} // namespace memoria
