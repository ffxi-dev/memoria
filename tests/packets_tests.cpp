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

TEST_CASE("zone changes, the Mog House and a cancelled logout keep the character in the game")
{
    CHECK(capture::packets::leavesGame(1));
    CHECK_FALSE(capture::packets::leavesGame(0));
    CHECK_FALSE(capture::packets::leavesGame(2)); // zone change
    CHECK_FALSE(capture::packets::leavesGame(3)); // Mog House
    CHECK_FALSE(capture::packets::leavesGame(4)); // cancelled
}

TEST_CASE("a packet's provenance says who injected, blocked or changed it")
{
    const std::array<uint8_t, 8> data{ 0x0A, 0x02, 0, 0, 1, 2, 3, 4 };
    auto                         changed = data;
    changed[5]                           = 9;
    CHECK(capture::packets::provenance(false, false, data.data(), nullptr, data.size()) == 0);
    CHECK(capture::packets::provenance(false, false, data.data(), data.data(), data.size()) == 0);
    CHECK(capture::packets::provenance(false, false, data.data(), changed.data(), data.size()) == xicap::PacketFlag::Modified);
    CHECK(capture::packets::provenance(true, true, data.data(), nullptr, data.size()) == (xicap::PacketFlag::Injected | xicap::PacketFlag::Blocked));
}
