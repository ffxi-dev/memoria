// The uploader, over WinHTTP, against a real Mogsafe instead of FakeMogsafe. Mogsafe's TestMemoriaUploads serves one
// and runs this test with MEMORIA_MOGSAFE (its URL), MEMORIA_MOGSAFE_TOKEN (an install token it knows), and
// MEMORIA_CAPTURE (a complete capture to send), then checks what it kept. Hidden, so a plain run skips it. Windows only.
#ifdef _WIN32
#include "plugin/winhttp.h"
#include "support.h"

#include <cstdlib>
#include <iostream>

using namespace memoria;
using namespace memoria::test;

namespace
{

auto env(const char* name) -> std::string
{
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read before any thread starts
    REQUIRE(value != nullptr);
    return value;
}

auto realUploader(const fs::path& dir, const std::string& server) -> std::unique_ptr<Uploader>
{
    UploadOptions options;
    options.dir    = dir;
    options.server = server;
    options.config = dir / "config";
    options.idle   = std::chrono::milliseconds(0);
    return std::make_unique<Uploader>(std::move(options), std::make_unique<plugin::WinHttp>(plugin::UPLOAD_TIMEOUTS));
}

} // namespace

TEST_CASE("a capture reaches a real Mogsafe whole, across a client restart", "[.mogsafe]")
{
    const auto server  = env("MEMORIA_MOGSAFE");
    const auto capture = readAll(env("MEMORIA_CAPTURE"));
    REQUIRE(capture.size() > 300);
    const auto dir  = tempDir();
    const auto file = dir / "a.xicap";
    REQUIRE(!saveLink(dir / "config", { .token = env("MEMORIA_MOGSAFE_TOKEN"), .account = "player" }));

    // The game closes after the header and a little more reached Mogsafe.
    const std::string bytes(capture.begin(), capture.end());
    append(file, bytes.substr(0, 300));
    {
        auto       up  = realUploader(dir, server);
        const auto err = up->uploadAll();
        INFO(err.value_or(""));
        CHECK(!err);
        REQUIRE(up->status().size() == 1);
        CHECK(up->status()[0].sent == 300);
    }

    // The next start finds the upload in uploads.txt and sends the rest from Mogsafe's offset, then closes it.
    append(file, bytes.substr(300));
    auto up = realUploader(dir, server);
    for (int i = 0; i < 3; ++i)
    {
        const auto err = up->uploadAll();
        INFO(err.value_or(""));
        CHECK(!err);
    }
    const auto status = up->status();
    REQUIRE(status.size() == 1);
    CHECK(status[0].sent == capture.size());
    CHECK(!status[0].refused);
    CHECK(status[0].error.empty());
    CHECK(!up->recordingId("a.xicap").empty());
    std::cout << "recording " << up->recordingId("a.xicap") << '\n';
    fs::remove_all(dir);
}
#endif
