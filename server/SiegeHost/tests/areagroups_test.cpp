// areagroups_test.cpp - the area group table, checked on the host compiler.
//
//   g++ -std=c++17 -O1 -Wall -Wextra -I../src -o areagroups_test areagroups_test.cpp && ./areagroups_test
//
// Two things are checked. First the shape of the table: no location listed twice, no group 0, group
// numbers contiguous from 1, and every group non-empty. Second, and this is the one that matters, the
// exact region-to-group mapping, place by place, written out here independently of
// the table so a typo in one does not hide a typo in the other.
//
// The list of locations is the map's own (MpWorld.dsmap
// world/maps/multiplayer_world/info/world_locations.gas, 71 entries). If a future map adds a place,
// kMapLocations below has to grow with it and this test will say so.
#include "areagroups.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <map>

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; printf("FAIL: %s\n", what); }
}
static void CheckGroup(unsigned loc, unsigned want, const char* name) {
    unsigned got = areagroups::ForLocation(loc);
    ++g_checks;
    if (got != want) { ++g_failed; printf("FAIL: %04u %-32s expected group %u, got %u\n", loc, name, want, got); }
}

// Every location in the map, and its expected area group.
struct Want { unsigned loc; unsigned group; const char* name; };
static const Want kMapLocations[] = {
    {  505,  1, "the town of Elddim" },
    { 1001,  2, "the town of Crystwind" },
    { 1204,  3, "the town of Fallraen" },
    {  202,  4, "the town of Meren" },
    {  408,  5, "the town of Lang" },
    {  301,  6, "the town of Quillrabe" },
    {  101,  7, "the town of Hiroth" },
    {  407,  8, "the town of Grescal" },

    {  501,  9, "Elddim Lowlands" },
    {  506,  9, "Elddim Lowlands (2nd id)" },
    {  503,  9, "Elddim Glade" },
    {  504,  9, "Long Glade" },
    {  507,  9, "the Orchard House Cellar" },
    {  701,  9, "Iliarth Falls" },
    {  702,  9, "Iliarth Divide" },
    {  705,  9, "Iliarth Valley" },

    { 1003, 10, "the Crystwind Mines" },
    { 1002, 10, "the Crystwind Mines (2nd id)" },
    { 1004, 10, "the Crystwind Old Mines" },
    { 1005, 10, "the Crystwind Grasslands" },
    { 1006, 10, "Miner's Haven" },

    { 1207, 11, "the Fallraen Barracks" },
    { 1203, 11, "Mt. Utrae Forest" },
    { 1205, 11, "the Glacial Caverns" },
    { 1206, 11, "a Furok cave" },
    { 1301, 11, "Fallraen Forest" },
    { 1302, 11, "the Frozen Rift" },
    { 1303, 11, "the Ancient Temple" },

    {  208, 12, "the Meren Coast" },
    {  201, 12, "the Cloud Forest" },
    {  209, 12, "the Mount Utrae Foothills" },
    {  204, 12, "the Mount Utrae north slope" },
    {  206, 12, "the Mount Utrae Alpine Reaches" },
    {  205, 12, "the Mount Utrae summit" },
    {  210, 12, "the Temple of Stars" },

    {  403, 13, "Lang Mire" },
    {  203, 13, "the Goblin Warrens" },
    {  207, 13, "the Goblin Outer Warrens" },

    {  302, 14, "Quillrabe Canyons" },
    {  704, 14, "Iliarth Canyon" },

    {  102, 15, "Castle Hiroth" },
    {  103, 15, "the Hiroth Mausoleum" },
    {  105, 15, "Hiroth Ravine" },

    {  401, 16, "the Mesa Desert" },
    {  402, 16, "the Endless Dunes" },
    {  406, 16, "the Abandoned Ruins" },
    {  801, 16, "the Volcanic Caverns" },
    {  903, 16, "the Lost Pyramid of the Dead" },

    {  601, 17, "Hovart's Folly (North)" },
    {  602, 17, "Hovart's Folly (South)" },
    {  603, 17, "Hovart's Folly Great Hall" },
    {  508, 17, "Hovart Marsh" },
    {  703, 17, "the Ancient Crypt" },

    {  104, 18, "the Drake Nest" },

    {  901, 19, "the Eastern Island" },
    {  902, 19, "the Lost Pyramid of the Spirits" },

    { 1101, 20, "the Utraean Basilicus" },

    { 1304, 21, "Mt. Elspen Forest" },
    { 1202, 21, "the Crystal Caverns" },
    { 1401, 21, "the Sulfur Tunnels" },
    { 1402, 21, "Dornek's Stone Quarry" },

    {  502, 22, "the Great Northern Forest" },

    {  303, 23, "the Fury Den" },
    {  304, 23, "Redwood Gap" },
    {  305, 23, "the Flooded Sanctuary" },
    {  410, 23, "The Abandoned Mine Shaft" },

    {  409, 24, "the Pit of Despair" },

    {  106, 25, "the Utraean Catacombs" },
    {  107, 25, "the Inner Sanctum" },

    {  306, 26, "the Forgotten Mesas" },
    {  307, 26, "the Trial of Gallus" },

    { 1500, 27, "Neener's Emporium" },

    {  922, 28, "the Ramparts" },
    { 1501, 28, "the Stronghold Gate" },
    { 1502, 28, "the Stronghold Tunnel" },

    {  923, 29, "the Zaurask Stronghold" },
    { 1503, 29, "the Stronghold" },
    { 1504, 29, "the Inner Stronghold" },
    { 1505, 29, "the Stronghold Caverns" },

    { 1506, 30, "the Great Clock Wastelands" },
    { 1507, 30, "the Tunnels of the Great Clock" },

    {  924, 31, "the Lands of the Great Clock" },
    { 1508, 31, "the Halls of the Great Clock" },
    { 1509, 31, "the Great Clock" },
};
static const int kWantCount = (int)(sizeof(kMapLocations) / sizeof(kMapLocations[0]));

int main() {
    // 1. the table's shape
    std::set<unsigned> seenLoc;
    std::map<unsigned, int> perGroup;
    bool dupes = false, zeroGroup = false;
    for (int i = 0; i < areagroups::kCount; ++i) {
        const areagroups::Entry& e = areagroups::kTable[i];
        if (!seenLoc.insert(e.loc).second) { dupes = true; printf("  duplicate location %04u\n", e.loc); }
        if (e.group == 0) { zeroGroup = true; printf("  location %04u has group 0\n", e.loc); }
        ++perGroup[e.group];
    }
    Check(!dupes, "no location appears twice in the table");
    Check(!zeroGroup, "no location is in group 0 (0 means 'place not known yet')");
    Check(areagroups::kCount == kWantCount, "table covers exactly the map's locations");
    if (areagroups::kCount != kWantCount)
        printf("  table has %d entries, the map has %d\n", areagroups::kCount, kWantCount);

    unsigned maxGroup = 0;
    for (std::map<unsigned, int>::const_iterator it = perGroup.begin(); it != perGroup.end(); ++it)
        if (it->first > maxGroup) maxGroup = it->first;
    bool contiguous = true;
    for (unsigned g = 1; g <= maxGroup; ++g)
        if (perGroup.find(g) == perGroup.end()) { contiguous = false; printf("  group %u is empty\n", g); }
    Check(contiguous, "groups run 1..N with no gaps and none empty");
    Check(maxGroup == 31, "31 groups");

    // 2. every location lands in its expected group
    for (int i = 0; i < kWantCount; ++i)
        CheckGroup(kMapLocations[i].loc, kMapLocations[i].group, kMapLocations[i].name);

    // 3. the two places that carry two ids must answer the same group through both of them
    Check(areagroups::ForLocation(501) == areagroups::ForLocation(506), "Elddim Lowlands: 0501 and 0506 agree");
    Check(areagroups::ForLocation(1002) == areagroups::ForLocation(1003), "Crystwind Mines: 1002 and 1003 agree");

    // 4. a town is never in the same group as its own surroundings or other towns
    Check(areagroups::ForLocation(505) != areagroups::ForLocation(501), "Elddim town is not its lowlands");
    Check(areagroups::ForLocation(1001) != areagroups::ForLocation(1005), "Crystwind town is not its grasslands");
    Check(areagroups::ForLocation(1500) != areagroups::ForLocation(1503), "Neener's Emporium is not Stronghold combat");
    Check(areagroups::ForLocation(1500) != areagroups::ForLocation(505),  "Neener's Emporium is not Elddim");
    Check(areagroups::ForLocation(1500) != areagroups::ForLocation(407),  "Neener's Emporium is not Grescal");

    // 5. town-only Shared Stash checks
    Check(areagroups::IsTownLocation(1500),  "Neener's Emporium (1500) is a town for /stash");
    Check(!areagroups::IsTownLocation(922),  "Ramparts (0922) is combat-only");
    Check(!areagroups::IsTownLocation(923),  "Stronghold (0923) is combat-only");
    Check(!areagroups::IsTownLocation(924),  "Lands of the Great Clock (0924) is combat-only");
    for (unsigned loc = 1501; loc <= 1509; ++loc)
        Check(!areagroups::IsTownLocation(loc), "LoA combat regions 1501..1509 block /stash");
    Check(areagroups::ForLocation(922) != areagroups::ForLocation(501), "Ramparts (0922) does not collide with Elddim Lowlands");

    // 6. the thing this release exists for
    Check(areagroups::ForLocation(408) != areagroups::ForLocation(407), "Lang and Grescal are no longer together");

    // 7. edge cases
    Check(areagroups::ForLocation(0) == 0, "0 stays 0 - the place is not known yet");
    Check(areagroups::ForLocation(9999) == 99, "an id the map does not have falls back to the old loc/100 rule");

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
