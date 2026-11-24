// hosthide_test.cpp - unit tests for hosthide_logic.h (ServerHost hiding rules).
#include "../src/hosthide_logic.h"
#include <cstdio>
#include <cstdlib>

static int g_pass = 0, g_fail = 0;

#define CHECK(expr) do { \
    if (expr) { ++g_pass; } \
    else { ++g_fail; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); } \
} while (0)

static void TestNameMatching() {
    CHECK(hosthide::MatchesHostName(L"ServerHost", nullptr, L"ServerHost"));
    CHECK(hosthide::MatchesHostName(nullptr, L"ServerHost", L"ServerHost"));
    CHECK(hosthide::MatchesHostName(L"serverhost", L"Other", L"ServerHost"));
    CHECK(hosthide::MatchesHostName(L"Other", L"SERVERHOST", L"ServerHost"));
    CHECK(!hosthide::MatchesHostName(L"Hero", L"Hero", L"ServerHost"));
    CHECK(!hosthide::MatchesHostName(L"", L"", L"ServerHost"));
    CHECK(!hosthide::MatchesHostName(nullptr, nullptr, L"ServerHost"));
    CHECK(!hosthide::MatchesHostName(L"ServerHost", L"ServerHost", L""));
    CHECK(!hosthide::MatchesHostName(L"ServerHost", L"ServerHost", nullptr));
    CHECK(!hosthide::MatchesHostName(L"ServerHost2", L"MyServerHost", L"ServerHost"));
}

static void TestUiFiltering() {
    // Hidden host on client: ServerHost skipped, real players kept.
    CHECK(!hosthide::ShouldShowInUi(true, true, false, true));
    CHECK(hosthide::ShouldShowInUi(true, true, false, false));
    // Local player is never hidden even if misconfigured with same name.
    CHECK(hosthide::ShouldShowInUi(true, true, true, true));
    // Invalid player always false.
    CHECK(!hosthide::ShouldShowInUi(false, true, false, false));
    CHECK(!hosthide::ShouldShowInUi(false, false, true, false));
    // ShowServerHost=1 restores visibility.
    CHECK(hosthide::ShouldShowInUi(true, false, false, true));
}

static void TestLocationChatSuppression() {
    CHECK(hosthide::ShouldSuppressLocationChat(true, false, true));
    CHECK(!hosthide::ShouldSuppressLocationChat(true, false, false));
    CHECK(!hosthide::ShouldSuppressLocationChat(true, true, true));
    CHECK(!hosthide::ShouldSuppressLocationChat(false, false, true));
}

static void TestRanksRowCount() {
    // 1 real player + ServerHost => 1 row (no blank row)
    CHECK(hosthide::ComputeRanksRowCount(1, 2, true, 8) == 1);
    // 3 real players + ServerHost => 3 rows
    CHECK(hosthide::ComputeRanksRowCount(3, 4, true, 8) == 3);
    // 0 visible players momentarily => clamped to 1 (prevents [-4] OOB read at 0x00515770)
    CHECK(hosthide::ComputeRanksRowCount(0, 1, true, 8) == 1);
    // 15 real players on MMO shard => clamped to 8 (prevents 8-slot array overflow)
    CHECK(hosthide::ComputeRanksRowCount(15, 16, true, 8) == 8);
    // ShowServerHost=1 => uses raw count (still clamped to [1, 8])
    CHECK(hosthide::ComputeRanksRowCount(1, 2, false, 8) == 2);
    CHECK(hosthide::ComputeRanksRowCount(1, 12, false, 8) == 8);
}

static void TestAspectCloakFlags() {
    // Typical visible, shadow-casting, selectable, collidable hero flags
    uint32_t stock = hosthide::kAspectVisible | hosthide::kAspectDrawShadow |
                     hosthide::kAspectDrawSelIndicator | hosthide::kAspectCollidable |
                     hosthide::kAspectBlocksPath | hosthide::kAspectSelectable |
                     0x00010000u;  // dynamically_lit
    CHECK(hosthide::NeedsAspectCloak(stock));
    uint32_t cloaked = hosthide::ComputeCloakedAspectFlags(stock);
    CHECK((cloaked & hosthide::kAspectVisible) == 0);
    CHECK((cloaked & hosthide::kAspectDrawShadow) == 0);
    CHECK((cloaked & hosthide::kAspectDrawSelIndicator) == 0);
    CHECK((cloaked & hosthide::kAspectCollidable) == 0);
    CHECK((cloaked & hosthide::kAspectBlocksPath) == 0);
    CHECK((cloaked & hosthide::kAspectSelectable) == 0);
    CHECK((cloaked & hosthide::kAspectInvincible) != 0);
    // Bit 0x10 MUST remain untouched (setting it would bypass FUN_00535b7c invisibility return!)
    CHECK((cloaked & 0x00000010u) == 0);
    // Unrelated flags preserved
    CHECK((cloaked & 0x00010000u) != 0);
    // Idempotent once cloaked
    CHECK(!hosthide::NeedsAspectCloak(cloaked));
    CHECK(hosthide::ComputeCloakedAspectFlags(cloaked) == cloaked);
}

int main() {
    TestNameMatching();
    TestUiFiltering();
    TestLocationChatSuppression();
    TestRanksRowCount();
    TestAspectCloakFlags();
    std::printf("hosthide_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
