// chatfix.cpp - 0.7.5, players' PCs only: chat lines stay 10 s, also while a menu is open.
//
// The in-game chat box (console_box in interface console_output, class chat_box) scrolls its oldest
// visible line away when its elapsed timer (+0x1E4) passes  line_timeout (+0x1D8) - offset (+0x224).
//   * line_timeout is 5.0 s at runtime.
//   * offset is set to 4.0 by the game's "console minimised" function (0x0050F79C) whenever a
//     panel such as the inventory, a shop or the map is open, and back to 0 when it closes - so with a
//     menu open a line lasted 5 - 4 = 1 s. Measured in the lab.
// Fix:
//   1. In the chat box update (0x006E6150) the load of the offset
//        006e619b  d9 86 24 02 00 00   fld dword [esi+0x224]
//      becomes  fldz + 4 nop,  so the timeout is the same with and without a menu open. Nothing else
//      reads the offset in this class; the game still writes it, it is simply ignored.
//   2. Once a second, the console box's line_timeout is set to [Chat] LineSeconds (default 10).
#include "siegehost.h"
#include "engine.h"
#include <cstdint>
#include <cstring>

namespace {
const DWORD kOffsetLoadVa = 0x006E619B;
const BYTE kOffsetLoadExpect[] = {0xD9, 0x86, 0x24, 0x02, 0x00, 0x00};
const BYTE kOffsetLoadNew[] = {0xD9, 0xEE, 0x90, 0x90, 0x90, 0x90};  // fldz; nop x4
const int kLineTimeout = 0x1D8;
bool g_on = false;
DWORD g_last = 0;
void* g_box = nullptr;

// 1.5.5: 3-second cooldown on "$MSG$%S does not have enough mana." (0x0078C614) so spamming casts
// does not flood the chat box. All 6 sites push 0x0078C614 and call ReportSys::MacroF (0x0042C62E).
const DWORD kManaReportCallee = 0x0042C62E;
const DWORD kManaSites[] = {0x005103FF, 0x0051082E, 0x00510B38, 0x00510E7F, 0x0051120A, 0x00513545};
const DWORD kManaCooldownMs = 3000;

// 1.5.5: "Player %S has left the game with all his worldly possessions." -> "their worldly possessions."
// Slot at 0x007916D8 is 72 bytes (padded with NULs up to 0x00791720); the updated string is 70 bytes incl. NUL.
const DWORD kLeaveHisVa = 0x007916D8;
const char kLeaveHisExpect[] = "$MSG$Player %S has left the game with all his worldly possessions.\n";
const char kLeaveTheirNew[]  = "$MSG$Player %S has left the game with all their worldly possessions.\n";
}

extern "C" char __cdecl SiegeHost_ManaMsgAllowed() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (last && now - last < kManaCooldownMs) return 0;
    last = now ? now : 1;
    return 1;
}

extern "C" void SiegeHost_ManaCooldownStub();
__asm__(
    ".text\n"
    ".globl _SiegeHost_ManaCooldownStub\n"
    "_SiegeHost_ManaCooldownStub:\n"
    "  call _SiegeHost_ManaMsgAllowed\n"
    "  testb %al, %al\n"
    "  jz 1f\n"
    "  movl $0x0042C62E, %eax\n"
    "  jmp *%eax\n"
    "1:\n"
    "  movl 0x007B2EA0, %eax\n"
    "  movl %fs:0x2C, %ecx\n"
    "  pushl %esi\n"
    "  movl (%ecx,%eax,4), %esi\n"
    "  movl $0x0042C680, %eax\n"
    "  jmp *%eax\n");

void ChatFixInstall() {
    // 1.5.5: update "his worldly possessions" -> "their worldly possessions" at 0x007916D8
    if (memcmp((void*)kLeaveHisVa, kLeaveHisExpect, sizeof kLeaveHisExpect) == 0) {
        DWORD o;
        if (VirtualProtect((void*)kLeaveHisVa, sizeof kLeaveTheirNew, PAGE_READWRITE, &o)) {
            memcpy((void*)kLeaveHisVa, kLeaveTheirNew, sizeof kLeaveTheirNew);
            VirtualProtect((void*)kLeaveHisVa, sizeof kLeaveTheirNew, o, &o);
            Log("ChatFix: updated leave message at 0x%08lX ('his' -> 'their' worldly possessions).", kLeaveHisVa);
        }
    }

    // 1.5.5: 3-second cooldown on out-of-mana chat messages (6 call sites)
    {
        bool manaOk = true;
        const BYTE kPushMana[] = {0x68, 0x14, 0xC6, 0x78, 0x00};
        for (DWORD s : kManaSites) {
            BYTE* p = (BYTE*)s;
            if (memcmp(p - 5, kPushMana, 5) != 0 || p[0] != 0xE8 || s + 5 + *(int32_t*)(p + 1) != kManaReportCallee) {
                manaOk = false;
                break;
            }
        }
        if (manaOk) {
            for (DWORD s : kManaSites) {
                BYTE* p = (BYTE*)s;
                DWORD o;
                if (VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) {
                    *(int32_t*)(p + 1) = (int32_t)((DWORD)(void*)&SiegeHost_ManaCooldownStub - (s + 5));
                    VirtualProtect(p, 5, o, &o);
                    FlushInstructionCache(GetCurrentProcess(), p, 5);
                }
            }
            Log("ChatFix: armed %lu ms cooldown on 'does not have enough mana' chat messages (6 call sites).", kManaCooldownMs);
        } else {
            Log("ChatFix: VERIFY FAILED on out-of-mana call sites - cooldown not installed.");
        }
    }

    const Config& c = Cfg();
    if (c.chatLineSeconds <= 0) { Log("ChatFix: off ([Chat] LineSeconds=0) - chat box left as the game sets it."); return; }
    if (memcmp((void*)kOffsetLoadVa, kOffsetLoadExpect, sizeof kOffsetLoadExpect) != 0) {
        Log("ChatFix: VERIFY FAILED at 0x%08lX - chat box left as the game sets it.", kOffsetLoadVa);
        return;
    }
    DWORD o;
    if (!VirtualProtect((void*)kOffsetLoadVa, sizeof kOffsetLoadNew, PAGE_EXECUTE_READWRITE, &o)) {
        Log("ChatFix: VirtualProtect failed (error %lu) - chat box left as the game sets it.", GetLastError());
        return;
    }
    memcpy((void*)kOffsetLoadVa, kOffsetLoadNew, sizeof kOffsetLoadNew);
    VirtualProtect((void*)kOffsetLoadVa, sizeof kOffsetLoadNew, o, &o);
    FlushInstructionCache(GetCurrentProcess(), (void*)kOffsetLoadVa, sizeof kOffsetLoadNew);
    g_on = true;
    Log("ChatFix: armed - chat lines stay %d s, with or without a menu open (patched 0x%08lX).", c.chatLineSeconds,
        kOffsetLoadVa);
}

void ChatFixTick() {
    if (!g_on) return;
    DWORD now = GetTickCount();
    if (now - g_last < 1000) return;
    g_last = now;
    void* w = eng::FindWindow("console_box", "console_output");
    if (!w) return;
    float want = (float)Cfg().chatLineSeconds;
    float* t = (float*)((char*)w + kLineTimeout);
    if (*t != want) {
        if (w != g_box) Log("ChatFix: chat box line time %.2f s -> %.2f s", *t, want);
        g_box = w;
        *t = want;
    }
}

// ---- 0.7.7: opening the chat shows the latest lines again, even ones that already timed out. ----
// The chat box never deletes timed-out lines (console_output.gas: delete_timeouts = false, max_history
// = 100); it scrolls them out of view by raising its top line (+0x1DC, vfuncs 0x18 get / 0x1C set).
// The game already has a history mode for that, bound to Page Up / Page Down ("chat_history_up/down",
// 0x006E706F / 0x006E70E5): it sets +0x1F8 = browsing, saves the top line in +0x1F4, steps the top line
// back (syncing the slider at +0x1C4 via 0x007029B5), and while browsing the box's timer is paused
// (0x006E6150 returns early). 0x006E715F ends it: restores the saved top line and resumes the timer.
// Here: when the chat edit box (console_edit in game_console_mp) opens, the same mode is entered with the
// top line set to show the last page (as many lines as fit, from 0x006E71B1); when it closes, the game's
// own 0x006E715F ends it, so lines that had timed out disappear again. [Chat] HistoryOnOpen=0 turns it off.
namespace {
const auto VisibleLines = (int(__fastcall*)(void* box))0x006E71B1;
const auto EndBrowsing = (void(__fastcall*)(void* box))0x006E715F;
const auto SliderSet = (void(__thiscall*)(void* slider, int pos))0x007029B5;
bool g_histOpen = false, g_histStarted = false;
void* g_histBox = nullptr;
int VCall0(void* box, int off) { return (*(int(__thiscall**)(void*))(*(char**)box + off))(box); }
void VSetTop(void* box, int v) { (*(void(__thiscall**)(void*, int))(*(char**)box + 0x1C))(box, v); }
}

void ChatHistoryTick() {
    if (!Cfg().chatHistoryOnOpen) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 100) return;  // 10 checks a second is plenty for a key press
    last = now;
    bool open = eng::WindowVisible("console_edit", "game_console_mp");
    if (open == g_histOpen) return;
    g_histOpen = open;
    void* box = eng::FindWindow("console_box", "console_output");
    if (!box) { g_histStarted = false; return; }
    char* b = (char*)box;
    if (open) {
        g_histStarted = false;
        if (b[0x1F8]) return;                       // the player is already browsing (Page Up): leave it
        int lines = VCall0(box, 0x10);              // all lines in the history
        int page = VisibleLines(box);
        int top = VCall0(box, 0x18);
        int target = lines - page; if (target < 0) target = 0;
        if (page <= 0 || target >= top) return;     // nothing hidden: nothing to show
        b[0x1F8] = 1;                               // exactly what chat_history_up does first
        *(int*)(b + 0x1F4) = top;
        while (VCall0(box, 0x18) > target) {
            VSetTop(box, VCall0(box, 0x18) - 1);
            if (*(void**)(b + 0x1C4)) SliderSet(*(void**)(b + 0x1C4), VCall0(box, 0x18));
        }
        g_histStarted = true; g_histBox = box;
        static int logged = 0;
        if (logged++ < 3) Log("ChatFix: chat opened - showing the last %d line(s) again (lines %d, top %d -> %d).",
                              top - target, lines, top, target);
    } else {
        if (g_histStarted && box == g_histBox && b[0x1F8]) EndBrowsing(box);  // the game's own end of history mode
        g_histStarted = false;
    }
}
