#include "../src/levelannounce_logic.h"
#include <cassert>
#include <cstdio>

using namespace levelannounce;

int main() {
    // 1. Join/load settle window suppresses initial high level & mid-load ramp
    {
        LevelTracker t(5000);
        ObservedPlayer p{1001, 0xAA01, "amom", "Aragorn", 0};
        assert(t.Update({p}, 1000).empty());

        // Character finishes loading level 69 within settle window -> silent!
        p.uberLevel = 69;
        assert(t.Update({p}, 2000).empty());
        assert(t.Update({p}, 5999).empty());

        // Reaches settle threshold (1000 + 5000 = 6000) -> settles at 69 silently
        assert(t.Update({p}, 6000).empty());

        // Levels up to 70 after settle -> announces!
        p.uberLevel = 70;
        auto ev = t.Update({p}, 7000);
        assert(ev.size() == 1);
        assert(ev[0].account == "amom");
        assert(ev[0].character == "Aragorn");
        assert(ev[0].oldLevel == 69);
        assert(ev[0].newLevel == 70);

        // Same level next tick -> no duplicate
        assert(t.Update({p}, 8000).empty());

        // Temporary dip -> ignored and does NOT re-trigger 70 later
        p.uberLevel = 68;
        assert(t.Update({p}, 9000).empty());
        p.uberLevel = 70;
        assert(t.Update({p}, 10000).empty());

        // Multi-level jump within sanity cap (70 -> 72) -> announces 72
        p.uberLevel = 72;
        ev = t.Update({p}, 11000);
        assert(ev.size() == 1);
        assert(ev[0].oldLevel == 70);
        assert(ev[0].newLevel == 72);

        // Huge corrupted memory spike (+20 levels in 1 second) -> suppressed
        p.uberLevel = 92;
        assert(t.Update({p}, 12000).empty());
    }

    // 2. Brand-new level 0 character leveling 0 -> 1 after settle
    {
        LevelTracker t(5000);
        ObservedPlayer p{2002, 0xBB02, "rookie", "Legolas", 0};
        assert(t.Update({p}, 100).empty());
        assert(t.Update({p}, 5100).empty()); // settled at 0
        p.uberLevel = 1;
        auto ev = t.Update({p}, 6100);
        assert(ev.size() == 1);
        assert(ev[0].oldLevel == 0 && ev[0].newLevel == 1);
    }

    // 3. Disconnect & reconnect resets tracker cleanly (no false announcement on rejoin)
    {
        LevelTracker t(5000);
        ObservedPlayer p{3003, 0xCC03, "bob", "Gimli", 40};
        assert(t.Update({p}, 0).empty());
        assert(t.Update({p}, 5000).empty());
        assert(t.TrackedCount() == 1);

        // Disconnect
        assert(t.Update({}, 6000).empty());
        assert(t.TrackedCount() == 0);

        // Reconnect with different character on same dpnid at level 80 -> silent!
        ObservedPlayer p2{3003, 0xDD04, "bob", "Gandalf", 80};
        assert(t.Update({p2}, 7000).empty());
        assert(t.Update({p2}, 12000).empty());
        assert(t.TrackedCount() == 1);
    }

    // 4. JSON escaping & payload formatting
    {
        LevelUpEvent ev{"am\"om", "Ara\\gorn\nHero", 69, 70};
        std::string j = BuildLevelUpJson(ev);
        std::string want = "{\"account\":\"am\\\"om\",\"character\":\"Ara\\\\gorn\\nHero\",\"level\":70,\"old_level\":69}";
        assert(j == want);
    }

    // 5. Portal URL parsing
    {
        std::string host, port;
        assert(ParsePortalUrl("http://72.60.164.124:8440", host, port));
        assert(host == "72.60.164.124" && port == "8440");

        assert(ParsePortalUrl("http://127.0.0.1:8440/api", host, port));
        assert(host == "127.0.0.1" && port == "8440");

        assert(ParsePortalUrl("http://example.com", host, port));
        assert(host == "example.com" && port == "80");

        assert(!ParsePortalUrl("https://example.com", host, port));
        assert(!ParsePortalUrl("", host, port));
    }

    printf("levelannounce_test: ALL PASSED\n");
    return 0;
}
