#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "support.h"

using namespace memoria;
using namespace memoria::test;

namespace
{

// Wine, which runs these tests on Linux, keeps no DACL on a file: the permission checks run on Windows itself.
auto keepsPermissions() -> bool
{
#ifdef _WIN32
    return ::GetProcAddress(::GetModuleHandleA("ntdll.dll"), "wine_get_version") == nullptr;
#else
    return true;
#endif
}

} // namespace

TEST_CASE("the link is kept across restarts")
{
    const auto dir    = tempDir();
    const auto config = dir / "config" / "memoria";
    CHECK(!loadLink(config));
    REQUIRE(!saveLink(config, { .token = "t0ken", .account = "player" }));
    const auto link = loadLink(config);
    REQUIRE(link);
    CHECK(link->token == "t0ken");
    CHECK(link->account == "player");
    CHECK((!keepsPermissions() || ownerOnly(config / "token")));
    REQUIRE(!saveLink(config, { .token = "n3w", .account = "player" })); // replaced whole, still private
    CHECK(loadLink(config)->token == "n3w");
    CHECK((!keepsPermissions() || ownerOnly(config / "token")));
    CHECK(std::distance(fs::directory_iterator(config), fs::directory_iterator{}) == 1);
    fs::remove_all(dir);
}

TEST_CASE("a link that cannot be written says why")
{
    const auto dir = tempDir();
    touch(dir / "config"); // a file where the folder should be
    const auto error = saveLink(dir / "config", { .token = "t0ken", .account = "player" });
    REQUIRE(error);
    CHECK(error->starts_with("cannot write "));
    fs::remove_all(dir);
}

TEST_CASE("live goes on only while the install shares: not paused, and the same token")
{
    const auto dir    = tempDir();
    const auto config = dir / "config";
    REQUIRE(!saveLink(config, { .token = "t0ken", .account = "player" }));
    CHECK(sharing(config, "t0ken"));
    REQUIRE(!setPaused(config, true)); // /memoria upload off, in any client
    CHECK(paused(config));
    CHECK_FALSE(sharing(config, "t0ken"));
    REQUIRE(!setPaused(config, false));
    CHECK(sharing(config, "t0ken"));
    REQUIRE(!saveLink(config, { .token = "t1", .account = "player" })); // linked again
    CHECK_FALSE(sharing(config, "t0ken"));
    REQUIRE(!unlink(config)); // /memoria unlink
    CHECK_FALSE(sharing(config, "t1"));
    CHECK(!unlink(config)); // nothing left to delete
    fs::remove_all(dir);
}

TEST_CASE("only the refused token is deleted")
{
    const auto dir = tempDir();
    CHECK(!removeLink(dir, "t0ken"));
    REQUIRE(!saveLink(dir, { .token = "n3w", .account = "player" }));
    CHECK(!removeLink(dir, "t0ken"));
    REQUIRE(loadLink(dir));
    CHECK(loadLink(dir)->token == "n3w");
    CHECK(removeLink(dir, "n3w"));
    CHECK(fs::is_empty(dir));
    fs::remove_all(dir);
}

TEST_CASE("a token taken by mistake goes back without replacing one saved meanwhile, where hard links fail")
{
    const auto dir  = tempDir();
    const auto olds = [&]
    {
        return std::ranges::count_if(fs::directory_iterator(dir), [](const auto& e)
                                     {
                                         return e.path().extension() == ".old";
                                     });
    };
    REQUIRE(!saveLink(dir, { .token = "a", .account = "player" }));
    CHECK(!removeLink(dir, "t0ken", [](const fs::path&, const fs::path&)
                      {
                          return false; // no hard links here
                      }));
    REQUIRE(loadLink(dir));
    CHECK(loadLink(dir)->token == "a");
    CHECK(olds() == 0);

    CHECK(!removeLink(dir, "t0ken", [&](const fs::path&, const fs::path&)
                      {
                          REQUIRE(!saveLink(dir, { .token = "n3w", .account = "player" })); // another client links meanwhile
                          return false;
                      }));
    REQUIRE(loadLink(dir));
    CHECK(loadLink(dir)->token == "n3w");
    CHECK(olds() == 1); // the taken token stays in its file
    fs::remove_all(dir);
}

TEST_CASE("the import reminder goes to one caller, ever")
{
    const auto dir = tempDir();
    CHECK_FALSE(importReminded(dir));
    CHECK(markImportReminded(dir));
    CHECK(importReminded(dir));
    CHECK_FALSE(markImportReminded(dir));
    fs::remove_all(dir);
}

TEST_CASE("upload.ini names another server only over HTTPS")
{
    const auto dir = tempDir();
    CHECK(!serverIn(dir));
    std::ofstream(dir / "upload.ini") << "; test server\n  server = https://mogsafe.test/ \r\n";
    CHECK(serverIn(dir) == "https://mogsafe.test");
    std::ofstream(dir / "upload.ini") << "server=http://mogsafe.test\n";
    CHECK(!serverIn(dir));
    fs::remove_all(dir);
}

TEST_CASE("the daily reminder goes to one caller a day")
{
    const auto dir = tempDir();
    CHECK(claimDailyReminder(dir));
    CHECK_FALSE(claimDailyReminder(dir));
    std::ofstream(dir / "reminded") << "2020-01-01"; // a reminder given on another day
    CHECK(claimDailyReminder(dir));
    fs::remove_all(dir);
}
