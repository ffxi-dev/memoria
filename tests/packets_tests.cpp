#include "core/capture/packets.h"
#include "support.h"

using namespace memoria;

TEST_CASE("a login packet names its character; one too short names nobody")
{
    std::vector<uint8_t> login(148);
    const uint32_t       id{ 0x01020304 };
    std::memcpy(login.data() + 4, &id, sizeof(id));
    std::memcpy(login.data() + 132, "Shantotto", 9);

    const auto who = capture::packets::character(login.data(), static_cast<uint32_t>(login.size()));
    CHECK(who.characterId == id);
    CHECK(who.name == "Shantotto");

    std::memset(login.data() + 132, 'A', 16); // a 16-letter name fills the field and has no terminator
    CHECK(capture::packets::character(login.data(), static_cast<uint32_t>(login.size())).name == std::string(16, 'A'));

    const auto none = capture::packets::character(login.data(), static_cast<uint32_t>(login.size() - 1));
    CHECK(none.characterId == 0);
    CHECK(none.name.empty());
}

TEST_CASE("only a logout that takes the character out of the game leaves it")
{
    CHECK(capture::packets::leavesGame(1));
    CHECK_FALSE(capture::packets::leavesGame(0));
    CHECK_FALSE(capture::packets::leavesGame(2)); // zone change
    CHECK_FALSE(capture::packets::leavesGame(3)); // Mog House
    CHECK_FALSE(capture::packets::leavesGame(4)); // cancelled
}
