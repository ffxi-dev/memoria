// Windows only: Shift-JIS goes through the Windows code page.
#ifdef _WIN32
#include "plugin/text.h"
#include "support.h"

using namespace memoria;

TEST_CASE("painted text goes back to the normal colour after it")
{
    CHECK(plugin::paint(plugin::Color::Red, "refused") == "\x1E\x44refused\x1E\x01");
}

TEST_CASE("counts group their thousands")
{
    CHECK(plugin::countText(0) == "0");
    CHECK(plugin::countText(999) == "999");
    CHECK(plugin::countText(1000) == "1,000");
    CHECK(plugin::countText(1234567) == "1,234,567");
}

TEST_CASE("durations round down to seconds, minutes, or hours and minutes")
{
    CHECK(plugin::durationText(45) == "45 s");
    CHECK(plugin::durationText(60) == "1 min");
    CHECK(plugin::durationText(3599) == "59 min");
    CHECK(plugin::durationText(3600 + 5 * 60 + 59) == "1 h 5 min");
}

TEST_CASE("the rest of a command keeps its spaces and drops its quotes")
{
    CHECK(plugin::restAfter("/memoria session start C:\\My Captures\\a.xicap  ", "start") == "C:\\My Captures\\a.xicap");
    CHECK(plugin::restAfter("/memoria session start \"a b.xicap\"", "start") == "a b.xicap");
    CHECK(plugin::restAfter("/memoria session start   ", "start").empty());
    CHECK(plugin::restAfter("/memoria session start", "start").empty());
    CHECK(plugin::restAfter("/memoria status", "start").empty());
}

TEST_CASE("Shift-JIS text becomes UTF-8")
{
    CHECK(plugin::utf8FromShiftJis("note 1") == "note 1");
    CHECK(plugin::utf8FromShiftJis("\x83\x65\x83\x58\x83\x67") == "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88");
    CHECK(plugin::utf8FromShiftJis("").empty());
}
#endif
