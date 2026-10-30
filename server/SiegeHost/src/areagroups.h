// areagroups.h - which players count as being in the same place as you.
//
// Two features ask that question and they must always agree: the top-left portrait panel on a CLIENT
// (portraitfix.cpp, [UI] NearbyOnly) and monster life/XP scaling on the GAME SERVER (areascale.cpp).
// They get their answer from here, and only from here, so a client and a server can never disagree
// about who is nearby.
//
// Where the input comes from: every machine knows each player's last named place at Player+0xF4,
// written by Player::RCSetWorldLocation (0x0056BA42) - the same call that prints "X has entered Y".
// The ids are the map's own, from MpWorld.dsmap world/maps/multiplayer_world/info/world_locations.gas.
// 0 means "not known yet", for the few seconds after someone joins, and both features treat 0 as
// nearby-everyone rather than guessing.
//
// replaces it with this table, grouping connected areas logically.
//
// NOTHING IN THE GAME'S CONTENT CHANGES. The group number is our own private label: it is never sent
// over the network, never saved into a character, never shown to a player, and the engine has no idea
// it exists. The only thing that reads it is "is your group the same as mine". So the numbers are free
// to be anything except 0, which is reserved for "place not known yet".
//
// Reading an old log: before 0.8.1 a group number was just the first two digits of a location id, so
// "groups [5]" meant the 05xx block. From 0.8.1 it means group 5 in the table below, which is the town
// of Lang. Same number, different meaning, different day.
#pragma once

namespace areagroups {

struct Entry { unsigned loc; unsigned group; };

// The map has 71 named locations. Two of them are listed twice under different ids (Elddim Lowlands is
// 0501 and 0506, the Crystwind Mines are 1002 and 1003), and a player can be stamped with either, so
// both ids of each pair are here and both carry the same group.
const Entry kTable[] = {
    // 1-8: the eight towns, each on its own. Standing in a town is not the same as standing outside it.
    {  505,  1 },  // the town of Elddim
    { 1001,  2 },  // the town of Crystwind
    { 1204,  3 },  // the town of Fallraen
    {  202,  4 },  // the town of Meren
    {  408,  5 },  // the town of Lang
    {  301,  6 },  // the town of Quillrabe
    {  101,  7 },  // the town of Hiroth
    {  407,  8 },  // the town of Grescal

    // 9-16: what surrounds each of those towns, in the same order.
    {  501,  9 },  // Elddim Lowlands
    {  506,  9 },  // Elddim Lowlands (second id for the same place)
    {  503,  9 },  // Elddim Glade
    {  504,  9 },  // Long Glade
    {  507,  9 },  // the Orchard House Cellar
    {  701,  9 },  // Iliarth Falls
    {  702,  9 },  // Iliarth Divide
    {  705,  9 },  // Iliarth Valley

    { 1003, 10 },  // the Crystwind Mines
    { 1002, 10 },  // the Crystwind Mines (second id for the same place)
    { 1004, 10 },  // the Crystwind Old Mines
    { 1005, 10 },  // the Crystwind Grasslands
    { 1006, 10 },  // Miner's Haven

    { 1207, 11 },  // the Fallraen Barracks
    { 1203, 11 },  // Mt. Utrae Forest
    { 1205, 11 },  // the Glacial Caverns
    { 1206, 11 },  // a Furok cave
    { 1301, 11 },  // Fallraen Forest
    { 1302, 11 },  // the Frozen Rift
    { 1303, 11 },  // the Ancient Temple

    {  208, 12 },  // the Meren Coast
    {  201, 12 },  // the Cloud Forest
    {  209, 12 },  // the Mount Utrae Foothills
    {  204, 12 },  // the Mount Utrae north slope
    {  206, 12 },  // the Mount Utrae Alpine Reaches
    {  205, 12 },  // the Mount Utrae summit
    {  210, 12 },  // the Temple of Stars

    {  403, 13 },  // Lang Mire
    {  203, 13 },  // the Goblin Warrens
    {  207, 13 },  // the Goblin Outer Warrens

    {  302, 14 },  // Quillrabe Canyons
    {  704, 14 },  // Iliarth Canyon

    {  102, 15 },  // Castle Hiroth
    {  103, 15 },  // the Hiroth Mausoleum
    {  105, 15 },  // Hiroth Ravine

    {  401, 16 },  // the Mesa Desert
    {  402, 16 },  // the Endless Dunes
    {  406, 16 },  // the Abandoned Ruins
    {  801, 16 },  // the Volcanic Caverns
    {  903, 16 },  // the Lost Pyramid of the Dead

    // 17-26: everything with no town of its own.
    {  601, 17 },  // Hovart's Folly (North)
    {  602, 17 },  // Hovart's Folly (South)
    {  603, 17 },  // Hovart's Folly Great Hall
    {  508, 17 },  // Hovart Marsh
    {  703, 17 },  // the Ancient Crypt

    {  104, 18 },  // the Drake Nest

    {  901, 19 },  // the Eastern Island
    {  902, 19 },  // the Lost Pyramid of the Spirits

    { 1101, 20 },  // the Utraean Basilicus

    { 1304, 21 },  // Mt. Elspen Forest
    { 1202, 21 },  // the Crystal Caverns
    { 1401, 21 },  // the Sulfur Tunnels
    { 1402, 21 },  // Dornek's Stone Quarry

    {  502, 22 },  // the Great Northern Forest

    {  303, 23 },  // the Fury Den
    {  304, 23 },  // Redwood Gap
    {  305, 23 },  // the Flooded Sanctuary
    {  410, 23 },  // The Abandoned Mine Shaft

    {  409, 24 },  // the Pit of Despair

    {  106, 25 },  // the Utraean Catacombs
    {  107, 25 },  // the Inner Sanctum

    {  306, 26 },  // the Forgotten Mesas
    {  307, 26 },  // the Trial of Gallus
};
const int kCount = (int)(sizeof(kTable) / sizeof(kTable[0]));

// The group a player in this location belongs to.
//   0 in  -> 0 out: the place is not known yet, and both callers already treat 0 as "nearby everyone".
//   a location the table does not list -> loc / 100, the 0.7.6 rule. That only happens if a future map
//   adds a place, and the old rule is a sane neighbour for a new id rather than lumping it with 0.
inline unsigned ForLocation(unsigned loc) {
    if (!loc) return 0;
    for (int i = 0; i < kCount; ++i)
        if (kTable[i].loc == loc) return kTable[i].group;
    return loc / 100;
}

// True for the eight towns on Utraean Peninsula (groups 1..8, each town on its own group).
inline bool IsTownGroup(unsigned group) {
    return group >= 1 && group <= 8;
}

inline bool IsTownLocation(unsigned loc) {
    if (!loc) return false;
    for (int i = 0; i < kCount; ++i)
        if (kTable[i].loc == loc) return IsTownGroup(kTable[i].group);
    return false;
}

// How many groups the table defines, i.e. the highest group number in it.
inline unsigned GroupCount() {
    unsigned n = 0;
    for (int i = 0; i < kCount; ++i) if (kTable[i].group > n) n = kTable[i].group;
    return n;
}

}  // namespace areagroups
