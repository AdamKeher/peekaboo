#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

#include "../src/core/RetroKeyNames.h"

using adm::core::AllRetroKeyNames;
using adm::core::AsciiToKeyStroke;
using adm::core::KeyStroke;
using adm::core::LookupModifierKey;
using adm::core::LookupRetroKey;
using adm::core::RetroKeyName;

TEST(RetroKeyNamesTest, ResolvesTheKeysABootblockMenuNeeds)
{
    int code = 0;
    for (const char* name : {"Return", "Escape", "Space", "F1", "Up", "Down", "1", "a"})
        EXPECT_TRUE(LookupRetroKey(name, code)) << name;
}

TEST(RetroKeyNamesTest, MatchingIgnoresCaseSpacesUnderscoresAndHyphens)
{
    int a = 0, b = 0, c = 0, d = 0;
    ASSERT_TRUE(LookupRetroKey("PageUp", a));
    ASSERT_TRUE(LookupRetroKey("page up", b));
    ASSERT_TRUE(LookupRetroKey("PAGE_UP", c));
    ASSERT_TRUE(LookupRetroKey("page-up", d));
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c);
    EXPECT_EQ(a, d);
}

TEST(RetroKeyNamesTest, UnknownNamesAreRejected)
{
    int code = 0;
    EXPECT_FALSE(LookupRetroKey("", code));
    EXPECT_FALSE(LookupRetroKey("NotAKey", code));
    EXPECT_FALSE(LookupRetroKey("F99", code));
}

TEST(RetroKeyNamesTest, EveryListedNameResolvesAndRoundTrips)
{
    const auto names = AllRetroKeyNames();
    ASSERT_FALSE(names.empty());
    for (const auto& name : names) {
        int code = 0;
        ASSERT_TRUE(LookupRetroKey(name, code)) << name;
        const char* back = RetroKeyName(code);
        ASSERT_NE(back, nullptr) << name;
        // The canonical name for a code must itself resolve to that same code,
        // otherwise the schema would advertise a name the tool then rejects.
        int again = 0;
        ASSERT_TRUE(LookupRetroKey(back, again)) << back;
        EXPECT_EQ(again, code) << name;
    }
}

TEST(RetroKeyNamesTest, ListedNamesAreUnique)
{
    const auto names = AllRetroKeyNames();
    std::set<std::string> seen;
    for (const auto& n : names)
        EXPECT_TRUE(seen.insert(n).second) << "duplicate key name: " << n;
}

TEST(RetroKeyNamesTest, UnknownCodesHaveNoName)
{
    EXPECT_EQ(RetroKeyName(-1), nullptr);
    EXPECT_EQ(RetroKeyName(0x7FFFFFFF), nullptr);
}

TEST(RetroKeyNamesTest, MapsPlainAsciiToKeystrokes)
{
    KeyStroke ks{};
    ASSERT_TRUE(AsciiToKeyStroke('a', ks));
    EXPECT_FALSE(ks.shift);
    const int lowerA = ks.code;

    ASSERT_TRUE(AsciiToKeyStroke('A', ks));
    EXPECT_TRUE(ks.shift);
    // Shifted letters press the same physical key.
    EXPECT_EQ(ks.code, lowerA);

    ASSERT_TRUE(AsciiToKeyStroke(' ', ks));
    EXPECT_FALSE(ks.shift);
    ASSERT_TRUE(AsciiToKeyStroke('1', ks));
    EXPECT_FALSE(ks.shift);
}

TEST(RetroKeyNamesTest, MapsTheControlCharactersTypeTextNeeds)
{
    KeyStroke ks{};
    int expected = 0;

    ASSERT_TRUE(AsciiToKeyStroke('\n', ks));
    ASSERT_TRUE(LookupRetroKey("Return", expected));
    EXPECT_EQ(ks.code, expected);

    ASSERT_TRUE(AsciiToKeyStroke('\t', ks));
    ASSERT_TRUE(LookupRetroKey("Tab", expected));
    EXPECT_EQ(ks.code, expected);

    ASSERT_TRUE(AsciiToKeyStroke('\b', ks));
    ASSERT_TRUE(LookupRetroKey("Backspace", expected));
    EXPECT_EQ(ks.code, expected);
}

TEST(RetroKeyNamesTest, MapsShiftedPunctuationOnAUsLayout)
{
    KeyStroke plain{}, shifted{};
    ASSERT_TRUE(AsciiToKeyStroke('1', plain));
    ASSERT_TRUE(AsciiToKeyStroke('!', shifted));
    EXPECT_TRUE(shifted.shift);
    EXPECT_EQ(shifted.code, plain.code);

    ASSERT_TRUE(AsciiToKeyStroke(';', plain));
    ASSERT_TRUE(AsciiToKeyStroke(':', shifted));
    EXPECT_TRUE(shifted.shift);
    EXPECT_EQ(shifted.code, plain.code);
}

TEST(RetroKeyNamesTest, EveryPrintableAsciiCharacterIsTypable)
{
    KeyStroke ks{};
    for (char c = 0x20; c < 0x7F; ++c)
        EXPECT_TRUE(AsciiToKeyStroke(c, ks)) << "no keystroke for '" << c << "'";
}

TEST(RetroKeyNamesTest, UnmappableCharactersAreRejected)
{
    KeyStroke ks{};
    EXPECT_FALSE(AsciiToKeyStroke('\0', ks));
    EXPECT_FALSE(AsciiToKeyStroke('\x01', ks));
}

TEST(RetroKeyNamesTest, ResolvesModifierNames)
{
    int code = 0;
    for (const char* name : {"shift", "ctrl", "alt", "lshift", "rshift", "lctrl", "rctrl"})
        EXPECT_TRUE(LookupModifierKey(name, code)) << name;

    // Same normalisation rules as key names.
    int a = 0, b = 0;
    ASSERT_TRUE(LookupModifierKey("LSHIFT", a));
    ASSERT_TRUE(LookupModifierKey("l-shift", b));
    EXPECT_EQ(a, b);

    EXPECT_FALSE(LookupModifierKey("hyper", code));
    EXPECT_FALSE(LookupModifierKey("", code));
}
