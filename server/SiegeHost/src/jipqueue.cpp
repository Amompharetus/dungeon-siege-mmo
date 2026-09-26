// jipqueue.cpp - 0.8.6: a player who ticks "I'm ready to begin" gets into the world without waiting
// for anybody else who is still on the character screen.
//
// THE BUG, REPRODUCED IN THE LAB
// ---------------------------------------------------------------------------------------------------
// Two players in the staging screen. The second one ticks ready, reaches the "joining" screen, and
// stays there forever. Everyone already in the world freezes. Lab, one dedicated server and two
// clients, one of which never ticks ready:
//
//   world=ws_mp_ingame_jip  pause=1
//   [#2 jip=1 ws=ws_invalid(0)    ]   <- still on the character screen
//   [#3 jip=1 ws=ws_loaded_map(9) ]   <- ticked ready, map loaded, waiting to be let in
//   t=152.457 sim=9645 ... t=152.457 sim=10001     <- world clock frozen, sim still ticking
//
// THE CAUSE, READ OFF THE ENGINE
// ---------------------------------------------------------------------------------------------------
// FUN_004C81F0 is the server's join state machine. It starts by counting the players:
//
//   for each valid player:
//       if (player->0x55)                 // this player is joining
//           if (first) { common = player->0xCC; n = 1; }
//           else if (player->0xCC == common) ++n;
//   allSame = (joiners != 0 && joiners == n);        // EVERY joiner in the SAME world state
//
// and then, only `if (allSame)`, it completes the join: at common == 9 it spawns each joiner's party
// and moves them to ws_wait_for_begin, and at common == 0x0B it puts them in the world, clears their
// joining flag, and sweeps everybody already in the world from ws_mp_ingame_jip back to ws_mp_ingame.
//
// So the engine batches joiners and only ever finishes when they are in lockstep. One person who has
// not ticked ready blocks every person who has, indefinitely, and the world stays paused because a
// joiner is past the frontend (which is JipGuard behaving correctly - the pause belongs to the map
// load).
//
// THE FIX
// ---------------------------------------------------------------------------------------------------
// Five call sites inside that one function are redirected. Nothing is inserted, nothing is moved, no
// stack frame is reinterpreted; each stub is a small predicate that either calls the original through
// or skips it.
//
//   0x004C8258  Player::IsValid, in the counting loop
//               -> answers "no" for a joiner who is still on the character screen, so the lockstep
//                  test is taken over the joiners who are actually ready. This is the whole fix; the
//                  other four stop the engine's own loops from touching the people it now ignores.
//   0x004C8576  the party spawn            -> only for a joiner whose own state is ws_loaded_map
//   0x004C861A  set joiner ws_wait_for_begin -> ws_mp_ingame   -> only for a joiner at wait_for_begin
//   0x004C8623  Player::RCSetJIP(false)    -> only for a joiner at wait_for_begin
//   0x004C864E  set everyone else ws_mp_ingame_jip -> ws_mp_ingame (this ENDS the join episode)
//               -> skipped while any joiner is still on the character screen
//
// That last one is the load-bearing safety rule. The completion code is reachable only while the world
// is in ws_mp_ingame_jip; if the episode ended while somebody was still choosing a character, that
// person could never be let in afterwards. So the world stays in the join state until the last joiner
// is done, which costs nothing: JipGuard already keeps the world running whenever every remaining
// joiner is still in the frontend.
//
// Net effect: whoever ticks ready is spawned and put in the world on their own, the world unfreezes as
// soon as they are in, and anybody still choosing carries on choosing.
#include "siegehost.h"
#include "engine.h"
#include "jipguard.h"
#include <stdint.h>
#include <stdio.h>

namespace {

const DWORD kValidSite = 0x004C8258, kValidCallee = 0x00569054;
const DWORD kSpawnSite = 0x004C8576, kSpawnCallee = 0x0056A2C9;
const DWORD kSetJoinSite = 0x004C861A, kSetWsCallee = 0x0052296B;
const DWORD kJipOffSite = 0x004C8623, kJipOffCallee = 0x00569C2E;
const DWORD kSweepSite = 0x004C864E;
// The function's shared tail call. Several paths reach it, including one that ends the join episode
// outright; it is guarded by the same rule as the sweep. Its arguments are (machineId, from, to,
// double) exactly like every other call to 0x0052296B.
const DWORD kTailSite = 0x004C8770;

const int kPlayerJip = 0x55;        // Player+0x55: this player is joining (Player::RCSetJIP writes it)
const int kPlayerWs = 0xCC;         // Player+0xCC: that player's own world state
const int kPlayerKind = 0x14;       // Player+0x14: 1 = human, 2 = computer

const int kWsLoadedMap = 0x09, kWsWaitForBegin = 0x0B;

bool g_on = false, g_diag = false;
DWORD g_hidden = 0, g_spawned = 0, g_admitted = 0, g_sweepHeld = 0, g_lastSummary = 0, g_total = 0;

// A joiner who has not left the character screen yet. ws_invalid means they have not even chosen.
inline bool StillChoosing(void* player) {
    return JipPlayerStillPicking(*(int*)((char*)player + kPlayerWs));
}
inline bool IsJoiner(void* player) { return *(char*)((char*)player + kPlayerJip) != 0; }

// Is anybody still sitting on the character screen? While that is true the join episode must not end.
bool AnyoneStillChoosing() {
    void* srv = *(void**)0x007ACF70;
    if (!srv) return false;
    void** b = *(void***)((char*)srv + 4);
    void** e = *(void***)((char*)srv + 8);
    for (void** it = b; it && it != e; ++it) {
        void* p = *it;
        if (!p || !eng::Player_IsValid(p)) continue;
        if (IsJoiner(p) && StillChoosing(p)) return true;
    }
    return false;
}

}  // namespace

// ---- the five stubs' C halves ------------------------------------------------------------------

// In the counting loop only: a joiner who is still choosing is not counted, so the engine's "every
// joiner is in the same state" test is taken over the ones who are actually ready.
extern "C" char __cdecl SiegeHost_JipCountValid(void* player) {
    char v = (player && eng::Player_IsValid(player)) ? 1 : 0;
    if (!v || !g_on) return v;
    if (!IsJoiner(player)) return 1;
    if (!StillChoosing(player)) return 1;
    ++g_hidden;
    return 0;
}

extern "C" char __cdecl SiegeHost_JipAtLoadedMap(void* player) {
    if (!g_on) return 1;
    if (!player) return 0;
    bool ok = *(int*)((char*)player + kPlayerWs) == kWsLoadedMap;
    if (ok) { ++g_spawned; ++g_total;
        if (g_diag) Log("JIPQUEUE: spawning the party of a joiner who is ready, on their own.");
    }
    return ok ? 1 : 0;
}

extern "C" char __cdecl SiegeHost_JipAtWaitForBegin(void* player) {
    if (!g_on) return 1;
    if (!player) return 0;
    bool ok = *(int*)((char*)player + kPlayerWs) == kWsWaitForBegin;
    if (ok) ++g_admitted;
    return ok ? 1 : 0;
}

// The shared tail call. Only the "world leaves the join state" case is held; everything else that
// reaches this call site is passed straight through.
extern "C" char __cdecl SiegeHost_JipTailAllowed(int from, int to) {
    if (!g_on) return 1;
    if (!(from == 0x29 && to == 0x28)) return 1;
    if (!AnyoneStillChoosing()) return 1;
    ++g_sweepHeld;
    return 0;
}

// The episode-ending sweep. Held while anybody is still choosing a character.
extern "C" char __cdecl SiegeHost_JipMayEndEpisode() {
    if (!g_on) return 1;
    if (!AnyoneStillChoosing()) return 1;
    ++g_sweepHeld;
    return 0;
}

// ---- the stubs ----------------------------------------------------------------------------------
// Each one preserves everything, asks its predicate, and either tail-jumps to the original or returns
// having cleaned exactly the bytes the original would have cleaned.
//
// ESI is the player-vector cursor in both of the engine's loops, which is visible at every one of
// these call sites and is only ever read here.
extern "C" void* g_jipValidOrig  = (void*)kValidCallee;
extern "C" void* g_jipSpawnOrig  = (void*)kSpawnCallee;
extern "C" void* g_jipSetWsOrig  = (void*)kSetWsCallee;
extern "C" void* g_jipOffOrig    = (void*)kJipOffCallee;

extern "C" void SiegeHost_JipValidStub();
extern "C" void SiegeHost_JipSpawnStub();
extern "C" void SiegeHost_JipSetJoinStub();
extern "C" void SiegeHost_JipOffStub();
extern "C" void SiegeHost_JipSweepStub();
extern "C" void SiegeHost_JipTailStub();

__asm__(
    // Player::IsValid, cdecl, one stack argument, caller cleans. Straight replacement.
    ".globl _SiegeHost_JipValidStub\n"
    "_SiegeHost_JipValidStub:\n"
    "  pushl 4(%esp)\n"
    "  call _SiegeHost_JipCountValid\n"
    "  addl $4, %esp\n"
    "  ret\n"

    // The party spawn: thiscall, ECX = Player, one stack argument.
    ".globl _SiegeHost_JipSpawnStub\n"
    "_SiegeHost_JipSpawnStub:\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %ecx\n"
    "  call _SiegeHost_JipAtLoadedMap\n"
    "  addl $4, %esp\n"
    "  testb %al, %al\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  je 1f\n"
    "  jmp *_g_jipSpawnOrig\n"
    "1:\n"
    "  ret $4\n"

    // Move this joiner into the world: thiscall on WorldState, 20 bytes of arguments.
    ".globl _SiegeHost_JipSetJoinStub\n"
    "_SiegeHost_JipSetJoinStub:\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl (%esi)\n"
    "  call _SiegeHost_JipAtWaitForBegin\n"
    "  addl $4, %esp\n"
    "  testb %al, %al\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  je 1f\n"
    "  jmp *_g_jipSetWsOrig\n"
    "1:\n"
    "  ret $20\n"

    // Player::RCSetJIP(false): thiscall, ECX = Player, one stack argument.
    ".globl _SiegeHost_JipOffStub\n"
    "_SiegeHost_JipOffStub:\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %ecx\n"
    "  call _SiegeHost_JipAtWaitForBegin\n"
    "  addl $4, %esp\n"
    "  testb %al, %al\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  je 1f\n"
    "  jmp *_g_jipOffOrig\n"
    "1:\n"
    "  ret $4\n"

    // End of the episode: same shape as the joiner move, held while anybody is still choosing.
    ".globl _SiegeHost_JipSweepStub\n"
    "_SiegeHost_JipSweepStub:\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  call _SiegeHost_JipMayEndEpisode\n"
    "  testb %al, %al\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  je 1f\n"
    "  jmp *_g_jipSetWsOrig\n"
    "1:\n"
    "  ret $20\n"

    // The shared tail call: (machineId, from, to, double) sit above our two saved registers.
    ".globl _SiegeHost_JipTailStub\n"
    "_SiegeHost_JipTailStub:\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl 20(%esp)\n"          // to
    "  pushl 20(%esp)\n"          // from
    "  call _SiegeHost_JipTailAllowed\n"
    "  addl $8, %esp\n"
    "  testb %al, %al\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  je 1f\n"
    "  jmp *_g_jipSetWsOrig\n"
    "1:\n"
    "  ret $20\n");

// ---------------------------------------------------------------------------------------------
void JipQueueTick() {
    if (!g_on) return;
    DWORD now = GetTickCount();
    if (!g_lastSummary) g_lastSummary = now;
    if (now - g_lastSummary < 60000) return;
    g_lastSummary = now;
    if (!g_spawned && !g_admitted && !g_sweepHeld) return;
    Log("JipQueue: last minute %lu joiner(s) let in on their own, %lu still choosing when the join "
        "would otherwise have ended (%lu joiners let in since this server started).",
        g_spawned, g_sweepHeld, g_total);
    g_hidden = g_spawned = g_admitted = g_sweepHeld = 0;
}

std::string JipQueueHeartbeatLines() {
    if (!g_on) return "jipqueue=off\n";
    char b[96];
    snprintf(b, sizeof b, "jipqueue=on\njipqueue_letin=%lu\n", g_total);
    return b;
}

namespace {
bool Redirect(const char* what, DWORD site, DWORD callee, void* stub) {
    BYTE* p = (BYTE*)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t*)(p + 1) != callee) {
        Log("JipQueue: VERIFY FAILED at 0x%08lX (%s) - feature off, nothing patched.", site, what);
        return false;
    }
    DWORD o;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) {
        Log("JipQueue: VirtualProtect failed at 0x%08lX (%lu) - feature off.", site, GetLastError());
        return false;
    }
    *(int32_t*)(p + 1) = (int32_t)((DWORD)stub - (site + 5));
    VirtualProtect(p, 5, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}
}  // namespace

void JipQueueInstall() {
    if (Cfg().role != Role::Server) return;      // the server owns the join sequence
    char e[8];
    g_diag = GetEnvironmentVariableA("SIEGEHOST_JIPQDIAG", e, sizeof e) != 0;
    if (GetEnvironmentVariableA("SIEGEHOST_NO_JIPQUEUE", e, sizeof e)) {
        Log("JipQueue: off (SIEGEHOST_NO_JIPQUEUE) - a joiner who ticks ready still waits for everyone "
            "else on the character screen, as in 0.8.5.");
        return;
    }
    if (!Cfg().jipQueue) {
        Log("JipQueue: off ([AutoHost] ReadyFirst=0) - a joiner who ticks ready still waits for "
            "everyone else on the character screen, as in 0.8.5.");
        return;
    }
    // All six or none: a half-armed set would let the engine touch players it is no longer counting.
    struct { const char* what; DWORD site, callee; void* stub; } s[] = {
        { "counting loop",   kValidSite,   kValidCallee,  (void*)&SiegeHost_JipValidStub   },
        { "party spawn",     kSpawnSite,   kSpawnCallee,  (void*)&SiegeHost_JipSpawnStub   },
        { "joiner into world", kSetJoinSite, kSetWsCallee, (void*)&SiegeHost_JipSetJoinStub },
        { "clear joining flag", kJipOffSite, kJipOffCallee, (void*)&SiegeHost_JipOffStub   },
        { "end of join",     kSweepSite,   kSetWsCallee,  (void*)&SiegeHost_JipSweepStub   },
        { "shared tail",     kTailSite,    kSetWsCallee,  (void*)&SiegeHost_JipTailStub    },
    };
    for (int i = 0; i < 6; ++i) {
        BYTE* p = (BYTE*)s[i].site;
        if (p[0] != 0xE8 || s[i].site + 5 + *(int32_t*)(p + 1) != s[i].callee) {
            Log("JipQueue: VERIFY FAILED at 0x%08lX (%s) - feature off, nothing patched.",
                s[i].site, s[i].what);
            return;
        }
    }
    for (int i = 0; i < 6; ++i)
        if (!Redirect(s[i].what, s[i].site, s[i].callee, s[i].stub)) return;
    g_on = true;
    Log("JipQueue: armed - whoever ticks 'I'm ready to begin' is spawned and put into the world on "
        "their own, without waiting for anyone still on the character screen, and the join only ends "
        "once the last of them is in (6 call sites in the server's join sequence).");
}
