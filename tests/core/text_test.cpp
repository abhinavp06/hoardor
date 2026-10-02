#include "core/text.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using hoardor::core::sort_key;
using hoardor::core::split_values;

namespace {

// Sorts names by their keys, the way an index on the key column would.
std::vector<std::string> sorted(std::vector<std::string> names) {
    std::stable_sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return sort_key(a) < sort_key(b); });
    return names;
}

}

TEST(SortKey, NumbersSortNaturally) {
    EXPECT_EQ(sorted({"Track 10", "Track 9", "Track 1", "Track 100"}),
              (std::vector<std::string>{"Track 1", "Track 9", "Track 10", "Track 100"}));
    EXPECT_EQ(sorted({"S01E10", "S01E02", "S02E01"}), (std::vector<std::string>{"S01E02", "S01E10", "S02E01"}));
    EXPECT_LT(sort_key("2"), sort_key("10"));
}

TEST(SortKey, LeadingZerosDontMatter) {
    EXPECT_EQ(sort_key("Track 02"), sort_key("track 2"));
    EXPECT_EQ(sort_key("007"), sort_key("7"));
    EXPECT_EQ(sort_key("0"), sort_key("00"));
}

TEST(SortKey, AVeryLongNumberStillSorts) {
    const std::string big(30, '9');
    EXPECT_LT(sort_key("x 99"), sort_key("x " + big));
    EXPECT_LT(sort_key("x " + big), sort_key("x 1" + big));
}

TEST(SortKey, CaseAndArticlesAreIgnored) {
    EXPECT_EQ(sort_key("The Beatles"), sort_key("beatles"));
    EXPECT_EQ(sort_key("A Moon Shaped Pool"), sort_key("moon shaped pool"));
    EXPECT_EQ(sort_key("An Awesome Wave"), sort_key("awesome wave"));
    EXPECT_EQ(sorted({"The Beatles", "Abba", "Coldplay"}), (std::vector<std::string>{"Abba", "The Beatles", "Coldplay"}));
    EXPECT_EQ(sort_key("The"), "the");             // nothing after the article: kept
    EXPECT_NE(sort_key("Theory"), sort_key("ory"));  // "The" must be a separate word
    EXPECT_EQ(sort_key("Les Mis", {"les "}), sort_key("mis"));
}

TEST(SortKey, NumbersBeforeLetters_UnicodeKept) {
    EXPECT_LT(sort_key("1984"), sort_key("Abbey Road"));
    EXPECT_EQ(sort_key("Björk"), "bj\xC3\xB6rk");  // only ASCII is lowercased
    EXPECT_EQ(sort_key(""), "");
    EXPECT_EQ(sort_key("  Kid A  "), sort_key("kid a"));
}

TEST(SplitValues, SeparatorsTrimmingAndDuplicates) {
    EXPECT_EQ(split_values("Rock; Pop ;;rock", ";"), (std::vector<std::string>{"Rock", "Pop"}));
    EXPECT_EQ(split_values(std::string("A\0B", 3), std::string(";\0", 2)), (std::vector<std::string>{"A", "B"}));
    EXPECT_EQ(split_values("Hip-Hop/Rap", ";/"), (std::vector<std::string>{"Hip-Hop", "Rap"}));
    EXPECT_TRUE(split_values("  ", ";").empty());
    EXPECT_TRUE(split_values("", ";").empty());
}
