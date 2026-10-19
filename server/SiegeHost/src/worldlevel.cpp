// worldlevel.cpp - lower the level a world tier demands, on one shard only.
//
// Return to Arhok's veteran world asks for level 60. Its main.gas says so:
//
//     [worlds] { [regular] { required_level = 10; }
//                [veteran] { required_level = 60; }
//                [elite]   { required_level = 87; } }
//
// The Event Raid shard is for level 54 and up, and it wants the veteran monsters, because the world
// tier is what decides which object and template set the map loads (objects/veteran, index/veteran).
// The level is the only thing in the way.
//
// The map cannot be edited. A .dsmap is a Tank and a modified Tank hangs the game (RE-19), so the 60
// has to be changed in memory or not at all.
//
// Where. Four places refuse or mark a character that is under the world's level:
//
//     0x00483A39  "This character is below the level required for the chosen World."   selecting it
//     0x00484E14  "Your character needs to be at least level %0.0f to play in this world."   Ready
//     0x004851DD  the same, on Start Game
//     0x0048C5B6  "Characters must be level %0.0f or higher." under the row in the list
//
// All four read one number, and that number is parsed once, here:
//
//     005915A2  build a gpstring at [ebp-0x70]          <- the tier name, "regular"/"veteran"/"elite"
//     005915AA  read the node's name into it
//     005915D3  push "required_level"
//     005915D8  call 0x004445BD                          <- reads the number into [ebp-0x68]
//     005915E7  hand (name, value) to the world record
//
// So the number is corrected the moment it is read, before anything downstream has seen it, and every
// one of those four places then agrees without being touched. The parser's own frame still holds the
// tier name two slots away, which is what makes it safe to tell veteran from elite here.
//
// Scope. This is driven entirely by [World] VeteranRequiredLevel, which is absent everywhere except
// the Event Raid shard's own ini and the ini the launcher writes for a player who picks that shard. NA
// and EU never set it, so on those servers and for those players this file does nothing at all. It is
// deliberately not scoped by map name: the shard hosts one map, and a key that is simply not present is
// a stronger guarantee than a string comparison.
//
// Both halves need it. The refusals above run on the player's own machine, so a player on an older DLL
// would still be told 60. They get the new DLL from updates/ on their next launch, the same way they
// get everything else.

#include "siegehost.h"
#include <cstring>
#include <cstdint>
#include <string>
#include <cstdio>
#include "engine.h"

extern "C" void* g_gasReadTail = (void*)0x004445BD;   // the GAS "read this key" the call site was using
extern "C" void* g_gasFieldRead = (void*)0x0041A7AE;  // the by-name field reader the record loader uses
extern "C" void __cdecl SiegeHost_WorldLevelParsed(const void* nameStr, void* value);
extern "C" void __cdecl SiegeHost_WorldLevelRecorded(void* node, void* value);
extern "C" void SiegeHost_WorldLevelParse();
extern "C" void SiegeHost_WorldLevelRecord();

// Replaces the call at 0x005915D8. The original is __thiscall: this in ecx, three arguments on the
// stack, and it cleans them itself with ret 0xC.
//
//     005915CD  push 1
//     005915CF  lea eax, [ebp-0x68]      <- where the number is written
//     005915D2  push eax
//     005915D3  push 0x78E63C            <- "required_level"
//     005915D8  call 0x004445BD          <- this call
//
// Standing in for a call means the stack has to look to the original exactly as the call site left it,
// so the three arguments are copied above the frame we were entered with rather than calling straight
// through: calling straight through would leave our own return address between the arguments and the
// original's, which shifts every one of them by four bytes and makes its ret 0xC unwind the wrong
// slots. Three identical pushes do it, because each push shifts the next argument into the same place.
// Nothing is kept in a global here, so this is safe whatever order or depth the parser calls it in.
//
// Cleaning up is then ours: ret 0xC, because that is what the call site expects of the original. The
// original's leave restores the parser's ebp before returning, so the tier name at [ebp-0x70] and the
// number at [ebp-0x68] are both still there to be read, and eax still holds what the original returned
// because pushal saved it.
//
// The .text below is not decoration. A top-level asm block is emitted into whichever section the
// compiler was last in, and the line above this one puts that at .data, which the linker marks writable
// and NOT executable. It runs anyway today only because DSLOA.exe is a 2003 binary with no NXCOMPAT
// bit, so Windows leaves DEP off for the whole process under its default OptIn policy. A machine with
// DEP set to cover every program would fault the moment the stub was entered.
__asm__(
    ".text\n"
    ".globl _SiegeHost_WorldLevelParse\n"
    "_SiegeHost_WorldLevelParse:\n"
    "  pushl 0xc(%esp)\n"             // arg 3 (1), then arg 2 (&value), then arg 1 ("required_level")
    "  pushl 0xc(%esp)\n"
    "  pushl 0xc(%esp)\n"
    "  call *_g_gasReadTail\n"        // cleans the copy itself
    "  pushal\n"
    "  leal -0x68(%ebp), %edx\n"      // the number just parsed
    "  leal -0x70(%ebp), %eax\n"      // the tier name, as a gpstring
    "  pushl %edx\n"
    "  pushl %eax\n"
    "  call _SiegeHost_WorldLevelParsed\n"
    "  addl $8, %esp\n"
    "  popal\n"
    "  ret $0xc\n");

// The site above is not the number the game later asks about. It is read a second time, into the world
// record the rest of the engine reads from, by the record loader at 0x005215BE:
//
//     005215CC  lea esi, [edi+8]                 <- record.screen_name
//     005215DC  call 0x00521814                     reads "screen_name" into it
//     005215E1  add edi, 0x10                    <- record.description
//     005215EE  call 0x00521814                     reads "description" into it
//     005215F6  push 0x7B5E08                    <- the default, if the key is absent
//     005215FB  add eax, 0x18                    <- record.required_level
//     005215FE  push eax
//     005215FF  push 0x78E63C                    <- "required_level"
//     00521604  push 0
//     00521606  push ebx                         <- the gas node this world was read from
//     00521607  call 0x0041A7AE                  <- this call
//     0052160C  add esp, 0x14                       cdecl: the caller cleans, not the callee
//
// Same stack problem, same fix, with five arguments instead of three. The difference is who cleans: the
// original is cdecl, so the copy is ours to remove and the real five are left for the call site to clean
// as it always did, which makes this a plain ret.
__asm__(
    ".text\n"
    ".globl _SiegeHost_WorldLevelRecord\n"
    "_SiegeHost_WorldLevelRecord:\n"
    "  pushl 0x14(%esp)\n"            // five copies, last argument first
    "  pushl 0x14(%esp)\n"
    "  pushl 0x14(%esp)\n"
    "  pushl 0x14(%esp)\n"
    "  pushl 0x14(%esp)\n"
    "  call *_g_gasFieldRead\n"
    "  addl $0x14, %esp\n"            // the copy is ours
    "  pushal\n"
    "  movl 0x30(%esp), %edx\n"       // argument 4: where the number was written
    "  movl 0x24(%esp), %eax\n"       // argument 1: the gas node
    "  pushl %edx\n"
    "  pushl %eax\n"
    "  call _SiegeHost_WorldLevelRecorded\n"
    "  addl $8, %esp\n"
    "  popal\n"
    "  ret\n");

namespace {
const DWORD kParseSite = 0x005915D8, kGasReadVa = 0x004445BD;
const DWORD kRecordSite = 0x00521607, kFieldReadVa = 0x0041A7AE;
bool  g_on = false;
int   g_want = 0;
long  g_changed = 0, g_seen = 0, g_recSeen = 0;
float g_veteranAsked = 0.0f;              // what veteran asked for before anything was changed

// Redirects a 5-byte E8 call, after checking it still goes where this build expects.
bool Redirect(DWORD site, DWORD callee, void* to, const char* what) {
    BYTE* p = (BYTE*)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t*)(p + 1) != callee) {
        Log("WorldLevel: VERIFY FAILED at 0x%08lX (%s) - the world's own level is left as the map set it.",
            site, what);
        return false;
    }
    DWORD o;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) {
        Log("WorldLevel: VirtualProtect failed at 0x%08lX (%lu).", site, GetLastError());
        return false;
    }
    *(int32_t*)(p + 1) = (int32_t)((DWORD)to - (site + 5));
    VirtualProtect(p, 5, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}
}

extern "C" void __cdecl SiegeHost_WorldLevelParsed(const void* nameStr, void* value) {
    if (!g_on || !value) return;
    const char* name = nameStr ? *(const char* const*)nameStr : 0;
    if (!name || !*name) return;
    float* f = (float*)value;
    // The first few are logged whatever they are, because the one thing that must not be guessed is
    // which tier a number belongs to and how it is stored.
    if (g_seen < 6) {
        ++g_seen;
        Log("WorldLevel: parsed tier '%s' required_level = %.1f (raw int %d)", name, *f, *(int*)value);
    }
    if (_stricmp(name, "veteran") != 0) return;
    if (!(*f > 0.0f) || *f > 200.0f) return;          // not a level we recognise; leave it alone
    g_veteranAsked = *f;                              // what the world record hook matches on
    if (*f <= (float)g_want) return;                  // already at or below what we want
    Log("WorldLevel: veteran required_level %.0f -> %d for this shard.", *f, g_want);
    *f = (float)g_want;
    ++g_changed;
}

namespace {
bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const BYTE*)p + n <= (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
}
bool Printable(const char* s) {
    if (!Readable(s, 1)) return false;
    for (int i = 0; i < 32; ++i) {
        if (!Readable(s + i, 1)) return false;
        if (s[i] == 0) return i > 0;
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7E) return false;
    }
    return false;                                     // no terminator in 32 bytes: not a screen name
}
}

// The tier is not named here, and the node is not asked for its name. The first version of this did ask,
// by calling the parser's own GasNode::GetName on the node the record loader is handed, and the game
// froze before the first line could be logged: the two call sites are handed different classes, and the
// node here is not one that call understands. What identifies the tier instead is the number itself.
// The parse hook above has already seen this map's three tiers by name and recorded which number veteran
// asked for, so the record whose required_level is exactly that number is veteran's, and no other tier on
// this map shares it (regular asks 10, elite asks 87). If the parse hook has not run, nothing is changed.
extern "C" void __cdecl SiegeHost_WorldLevelRecorded(void* node, void* value) {
    (void)node;
    if (!g_on || !value || !Readable(value, 4)) return;
    float* f = (float*)value;
    if (g_recSeen < 6) {
        ++g_recSeen;
        const char* screen = Readable((BYTE*)value - 0x10, 4) ? eng::GpC((BYTE*)value - 0x10, 0) : "?";
        if (!Printable(screen)) screen = "?";
        Log("WorldLevel: world record screen_name '%s' required_level = %.1f", screen, *f);
    }
    if (g_veteranAsked <= 0.0f) return;               // the parse hook has not named a veteran number yet
    if (*f != g_veteranAsked) return;                 // some other tier's record
    if (*f <= (float)g_want) return;
    // The engine reloads this record constantly: a five minute lab run went through it 262 times. The
    // first few are worth seeing, the rest would bury the log, so the running total goes in the
    // heartbeat instead.
    if (g_changed < 3)
        Log("WorldLevel: veteran world record required_level %.0f -> %d for this shard.", *f, g_want);
    else if (g_changed == 3)
        Log("WorldLevel: the veteran world record is rewritten every time the engine reloads it; the "
            "running count is 'worldlevel_set' in heartbeat.txt and this is the last line about it.");
    *f = (float)g_want;
    ++g_changed;
}

std::string WorldLevelHeartbeatLines() {
    if (!g_on) return "worldlevel=off\n";
    char b[96];
    snprintf(b, sizeof b, "worldlevel=veteran:%d\nworldlevel_set=%ld\n", g_want, g_changed);
    return b;
}

void WorldLevelInstall() {
    g_want = Cfg().veteranRequiredLevel;
    if (g_want <= 0) return;                          // absent on NA, EU and every normal client
    bool a = Redirect(kParseSite, kGasReadVa, (void*)&SiegeHost_WorldLevelParse, "parse");
    bool b = Redirect(kRecordSite, kFieldReadVa, (void*)&SiegeHost_WorldLevelRecord, "world record");
    if (!a && !b) return;
    g_on = true;
    Log("WorldLevel: armed - a veteran world on this shard asks for level %d instead of whatever its map "
        "says (patched%s%s). Regular and elite are untouched, and no map file is modified.",
        g_want, a ? " 0x005915D8" : "", b ? " 0x00521607" : "");
}
