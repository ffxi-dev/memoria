#include "core/capture/recorder.h"

#include "core/file.h"

#include <picosha2.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <random>
#include <span>
#include <tuple>
#include <utility>

namespace memoria::capture
{
namespace
{

// Per character, since clients can share an install. It says whether the character's last capture was left open by a crash, so the next one can name it.
auto stateFile(const std::filesystem::path& dir, const xicap::Character& character) -> std::filesystem::path
{
    return dir / std::format("state-{}.txt", character.characterId);
}

constexpr auto   REOPEN_EVERY     = std::chrono::minutes(1);
constexpr size_t GAME_HEADER_SIZE = 4;

auto parseHex(const std::string_view text) -> std::optional<xicap::CaptureId>
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

// A login that could not read the content id and world (0) matches on the character id and name alone.
auto sameCharacter(const Login& a, const Login& b) -> bool
{
    const bool known{ a.character.contentId != 0 && b.character.contentId != 0 };
    return a.character.characterId == b.character.characterId && a.character.name == b.character.name && (!known || (a.character.contentId == b.character.contentId && a.worldId == b.worldId));
}

auto fileName(const uint64_t timestamp, const xicap::Character& character, const xicap::CaptureId& id) -> std::string
{
    const std::chrono::sys_time<std::chrono::milliseconds> when{ std::chrono::milliseconds{ timestamp } };

    auto name = character.name;
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
    event.kind = EventKind::Packet;
    queue_.push(std::move(event));
}

auto Recorder::login(const uint64_t timestamp, Login who) -> void
{
    queue_.push({ .kind = EventKind::Login, .timestamp = timestamp, .login = std::move(who) });
}

auto Recorder::logout(const uint64_t timestamp) -> void
{
    queue_.push({ .kind = EventKind::Logout, .timestamp = timestamp });
}

auto Recorder::note(const uint64_t timestamp, std::string text) -> void
{
    queue_.push({ .kind = EventKind::Note, .timestamp = timestamp, .text = std::move(text) });
}

auto Recorder::sessionStart(const uint64_t timestamp, const std::filesystem::path& path) -> void
{
    const auto utf8 = path.u8string();
    queue_.push({ .kind = EventKind::SessionStart, .timestamp = timestamp, .text = { utf8.begin(), utf8.end() } });
}

auto Recorder::sessionStop(const uint64_t timestamp) -> void
{
    queue_.push({ .kind = EventKind::SessionStop, .timestamp = timestamp });
}

auto Recorder::status() const -> RecorderStatus
{
    const std::scoped_lock lock(statusMutex_);
    return status_;
}

auto Recorder::stop() -> std::vector<std::filesystem::path>
{
    {
        const std::scoped_lock lock(wakeMutex_);
        if (stopping_)
        {
            return {};
        }
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
    const std::scoped_lock lock(wakeMutex_);
    return std::exchange(closedAtStop_, {});
}

// A session is optional: its failure ends it and leaves the main capture recording.
template <typename Op>
auto Recorder::onSession(const Op op) -> void
{
    if (!session_)
    {
        return;
    }
    try
    {
        op(*session_);
    }
    catch (const std::exception& error)
    {
        sessionFailed(error);
    }
}

auto Recorder::run() -> void
{
    std::vector<Event> events;
    while (true)
    {
        bool stopping{ false };
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
                const std::scoped_lock lock(statusMutex_);
                status_.dropped += dropped;
            }
            handle({ .kind = EventKind::Note, .text = std::format("dropped {} packets: the writer fell behind", dropped) });
        }

        const auto now = std::chrono::steady_clock::now();
        if (writer_ && now - lastFlush_ >= options_.flushEvery)
        {
            try
            {
                writer_->flush();
                wrote();
            }
            catch (const std::exception& error)
            {
                broken(error);
            }
            onSession([](xicap::Writer& session)
                      {
                          session.flush();
                      });
            lastFlush_ = now;
            if (writer_ && options_.ended && options_.ended(status().capture))
            {
                rollover();
            }
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
            case EventKind::Login:
                if (!event.login)
                {
                    break;
                }
                if (writer_ && login_ && sameCharacter(*login_, *event.login))
                {
                    login_ = event.login; // the header keeps what it had; later logins compare against what is now known
                    mark(xicap::MarkerKind::Login);
                }
                else
                {
                    close();
                    open(event.timestamp, *event.login);
                }
                break;
            case EventKind::Logout:
                mark(xicap::MarkerKind::Logout);
                break;
            case EventKind::Note:
                mark(xicap::MarkerKind::Note, event.text);
                break;
            case EventKind::SessionStart:
                openSession(std::u8string(event.text.begin(), event.text.end()));
                break;
            case EventKind::SessionStop:
                closeSession();
                break;
            case EventKind::Packet:
                if (event.channel == xicap::Channel::Game && event.data.size() < GAME_HEADER_SIZE) // the writer refuses it
                {
                    const std::scoped_lock lock(statusMutex_);
                    status_.dropped++;
                    break;
                }
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
                else if (login_) // the capture broke: until it reopens, the packet is lost
                {
                    const std::scoped_lock lock(statusMutex_);
                    status_.dropped++;
                }
                break;
        }
    }
    catch (const std::exception& error)
    {
        broken(error);
    }
}

auto Recorder::metadata(const Login& who, const xicap::CaptureId& previous) const -> xicap::Metadata
{
    return {
        .captureId         = randomCaptureId(),
        .previousCaptureId = previous,
        .serverKind        = who.serverKind,
        .captureAgent      = options_.captureAgent,
        .clientVersion     = who.clientVersion,
        .character         = who.character,
    };
}

// Called with no capture open. A session open from before goes on.
auto Recorder::open(const uint64_t timestamp, const Login& who) -> void
{
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
    chat_           = options_.chat ? options_.chat() : xicap::Chat::Redact;
    auto       meta = metadata(who, previous_);
    const auto path = options_.dir / fileName(timestamp, who.character, meta.captureId);
    login_          = who;
    reopenAt_       = std::chrono::steady_clock::now() + REOPEN_EVERY;
    writer_         = std::make_unique<xicap::Writer>(path, std::move(meta), xicap::FramePolicy::live(), chat_);
    previous_       = writer_->captureId();
    saveState(true);
    if (!who.midSession)
    {
        mark(xicap::MarkerKind::Login);
    }
    const std::scoped_lock lock(statusMutex_);
    status_.capture = path;
    status_.chat    = chat_;
    status_.packets = 0;
    status_.bytes   = 0;
    status_.search  = 0;
    status_.firstMs = timestamp;
    status_.lastMs  = timestamp;
}

auto Recorder::write(const Event& event) -> void
{
    const auto packet = xicap::WritePacket{
        .timestamp  = event.timestamp,
        .direction  = event.direction,
        .provenance = event.provenance,
        .channel    = event.channel,
        .opcode     = event.opcode,
        .data       = event.data,
    };
    onSession([&](xicap::Writer& session)
              {
                  session.write(packet);
              });
    writer_->write(packet);
    live_.push(packet, chat_);
    const std::scoped_lock lock(statusMutex_);
    status_.packets++;
    status_.bytes += event.data.size();
    if (event.channel == xicap::Channel::Search)
    {
        status_.search++;
    }
    status_.lastMs = event.timestamp;
}

auto Recorder::mark(const xicap::MarkerKind kind, const std::string_view text) -> void
{
    onSession([&](xicap::Writer& session)
              {
                  session.mark(kind, text);
              });
    if (writer_)
    {
        writer_->mark(kind, text);
    }
}

auto Recorder::openSession(const std::filesystem::path& path) -> void
{
    closeSession();
    bool created{ false };
    try
    {
        if (!writer_)
        {
            throw std::runtime_error("no capture is open, so no session either");
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        // Created here, never replaced: an existing file, the main capture among them, refuses the session.
        created = createNew(path);
        if (!created)
        {
            throw std::runtime_error(std::filesystem::exists(path, ec) ? std::format("{} exists, and a session never replaces a file", path.string()) : std::format("cannot create {}", path.string()));
        }
        session_ = std::make_unique<xicap::Writer>(path, metadata(*login_, writer_->captureId()), xicap::FramePolicy::live(), chat_);
    }
    catch (const std::exception& error)
    {
        if (created)
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        sessionFailed(error);
        return;
    }
    const std::scoped_lock lock(statusMutex_);
    status_.session = path;
    status_.sessionError.clear();
}

auto Recorder::closeSession() -> void
{
    onSession([](xicap::Writer& session)
              {
                  session.close();
              });
    session_.reset();
    const std::scoped_lock lock(statusMutex_);
    status_.session.clear();
}

auto Recorder::sessionFailed(const std::exception& error) -> void
{
    session_.reset();
    const std::scoped_lock lock(statusMutex_);
    status_.sessionError = error.what();
    status_.session.clear();
}

auto Recorder::close() -> void
{
    closeSession();
    if (!writer_)
    {
        return;
    }
    finish();
    login_.reset();
    const std::scoped_lock lock(statusMutex_);
    status_.capture.clear();
}

auto Recorder::fail(const std::exception& error) -> void
{
    const std::scoped_lock lock(statusMutex_);
    status_.error = error.what();
}

// A writer that failed once stays failed. Dropping it leaves the capture marked open, as a crash would, and the next packet starts a new one.
auto Recorder::broken(const std::exception& error) -> void
{
    fail(error);
    session_.reset();
    writer_.reset();
    {
        const std::scoped_lock lock(statusMutex_);
        if (!status_.session.empty())
        {
            status_.sessionError = std::format("the capture stopped: {}", error.what());
        }
        status_.capture.clear();
        status_.session.clear();
    }
    if (!std::exchange(failing_, true) && options_.failed)
    {
        options_.failed(error.what());
    }
}

// The server ended the open capture: close it here, keep the login and session, and let the next packet start a capture
// that names this one. The uploader sends this capture's last bytes as a -rest capture of their own.
auto Recorder::rollover() -> void
{
    finish();
    reopenAt_ = {};
    const std::scoped_lock lock(statusMutex_);
    status_.capture.clear();
}

// Writes the trailer and drops the writer. A trailer that failed leaves the capture cut off, so the state stays open and the next capture names it.
auto Recorder::finish() -> void
{
    {
        const std::scoped_lock lock(wakeMutex_);
        if (stopping_)
        {
            closedAtStop_.push_back(status().capture);
        }
    }
    bool closed{ true };
    try
    {
        writer_->close();
    }
    catch (const std::exception& error)
    {
        fail(error);
        closed = false;
    }
    saveState(!closed);
    writer_.reset();
    if (closed)
    {
        wrote();
    }
}

// A capture reached disk: the last error no longer applies, and the next failure is said again.
auto Recorder::wrote() -> void
{
    failing_ = false;
    const std::scoped_lock lock(statusMutex_);
    status_.error.clear();
}

// Replaced whole, so a crash mid-write leaves the old state.
auto Recorder::saveState(const bool open) -> void
{
    if (login_)
    {
        std::ignore = replaceFile(stateFile(options_.dir, login_->character), std::format("{} {}\n", picosha2::bytes_to_hex_string(previous_), open ? "open" : "closed"));
    }
}

} // namespace memoria::capture
