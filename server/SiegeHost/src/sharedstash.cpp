// sharedstash.cpp - Account-wide Shared Stash for Dungeon Siege MMO
#include "sharedstash.h"
#include "characterabilities.h"
#include "siegehost.h"
#include "engine.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

const uint32_t kStashMagic    = 0x53545348; // 'STSH'
const uint32_t kPkt_ReqOpen   = 1;
const uint32_t kPkt_StashInfo = 2;

#pragma pack(push, 1)
struct StashPacket {
    uint32_t magic;      // kStashMagic
    uint32_t type;       // kPkt_*
    uint32_t heroGoid;
    uint32_t stashGoid;
};
#pragma pack(pop)

struct QueuedStashPacket {
    bool        isServer;
    DWORD       senderDpnid;
    StashPacket pkt;
};

// Engine function pointers
typedef bool (__cdecl* IsServerLocalFn)();
static const auto s_isServerLocal = (IsServerLocalFn)0x00450B04;

typedef float (__thiscall* GetSkillLevelFn)(void*, const char*);
static const auto s_getSkillLevel = (GetSkillLevelFn)0x0047FC6E;

typedef void (__thiscall* PlayVoiceFn)(void*, const char*, bool);
static const auto s_playVoice = (PlayVoiceFn)0x00539D77;

typedef void* (__thiscall* FindPartyMemberFn)(void* uiPartyMgr, uint32_t goid, int* outSlotIdx);
static const auto s_findPartyMember = (FindPartyMemberFn)0x004A4FC5;

typedef void (__thiscall* OpenCharInventoryFn)(void* uiPartyMgr, int slotIdx, char deselectOthers, int unk);
static const auto s_openCharInventory = (OpenCharInventoryFn)0x0049BB63;

typedef void (__thiscall* ShowCharInventoryGridFn)(void* uiPartyMgr, char open, int unk);
static const auto s_showCharInventoryGrid = (ShowCharInventoryGridFn)0x0049C5AC;

typedef int (__thiscall* ToggleSpellbookFn)(void* uiPartyMgr, int open);
static const auto s_toggleSpellbook = (ToggleSpellbookFn)0x0049CB29;

typedef void (__thiscall* ToggleBackpackFn)(void* uiPartyMgr, int open, char unk);
static const auto s_toggleBackpack = (ToggleBackpackFn)0x0049D0F6;

typedef void (__thiscall* SetTextFn)(void*, const eng::gpstring*, bool);
static const auto s_setText = (SetTextFn)0x0047B4C4;

typedef void* (__thiscall* FindGoFn)(void* godb, DWORD goid);
static const auto s_findGo = (FindGoFn)0x00550A6B;

typedef const char* (__thiscall* TemplateNameFn)(void* go);
static const auto s_templateName = (TemplateNameFn)0x005390A3;

typedef void (__thiscall* SMarkGoAndChildrenForDeletionFn)(void* godb, void* go);
static const auto s_sMarkGoAndChildrenForDeletion = (SMarkGoAndChildrenForDeletionFn)0x004715F8;

typedef void (__thiscall* SSetGoldValueFn)(void*, int, char);
static const auto s_setGoldValue = (SSetGoldValueFn)0x00533F7F;

typedef void (__thiscall* CloseConsoleFn)(void*);
static const auto s_closeConsole = (CloseConsoleFn)0x004EF100;

typedef const void* (__thiscall* GetPositionFn)(void* placement);
static const auto s_getPosition = (GetPositionFn)0x00540DFE;

typedef void* (__cdecl* MakeCloneReqFn)();
static const auto s_makeCloneReq = (MakeCloneReqFn)0x005459B2;

typedef void (__thiscall* SetStartingPosFn)(void* req, const void* pos);
static const auto s_setStartingPos = (SetStartingPosFn)0x00544F77;

typedef void (__thiscall* SetForceClientAllowedFn)(void* req, bool allowed);
static const auto s_setForceClientAllowed = (SetForceClientAllowedFn)0x0047BCD1;

typedef uint32_t (__thiscall* SCloneGoFn)(void* godb, const void* req, const char* templateName);
static const auto s_sCloneGo = (SCloneGoFn)0x0054A356;

typedef uint32_t (__thiscall* SCloneGoWithContentFn)(void* godb, const void* req, const char* pcontent, int zero);
static const auto s_sCloneGoWithContent = (SCloneGoWithContentFn)0x0054A457;

typedef void* (__thiscall* RSAddFn)(void* inv, void* go, int loc, int origin, bool autoPlace);
static const auto s_rsAdd = (RSAddFn)0x0057545B;

typedef void (__thiscall* MapFindFn)(void* mapThis, void** outNode, void** keyPtr);
static const auto s_mapFind = (MapFindFn)0x0057F023;

CRITICAL_SECTION g_stashPktCs;
bool g_stashPktCsInit = false;
QueuedStashPacket g_stashPktQueue[32] = {};
int g_stashPktHead = 0;
int g_stashPktTail = 0;
void* g_dp8Client = nullptr;
void* g_dp8Server = nullptr;

bool g_sharedStashActive = false;
bool g_wasInventoryOpen = false;
bool g_pendingOpenStash = false;
bool g_eatNextReturnChar = false;
bool g_suppressBackpackSfx = false;
uint32_t g_localStashGoid = 0;
int g_lastTitleMode = 0; // 0 = unset, 1 = Shared Stash, 2 = Backpack
DWORD g_lastTitleUpdate = 0;

struct StashItemRecord {
    std::string template_name;
    std::string variation_name;
    int gold_value{0};
    std::string modifier_prefix_name;
    std::string modifier_suffix_name;
    std::vector<std::pair<std::string, int>> spells;
};

struct AccountStash {
    std::string account;
    uint32_t heroGoid{0};
    uint32_t stashGoid{0};
    void* stashGo{nullptr};
    uint32_t lastHash{0};
    int lastItemCount{-1};
    DWORD lastSavedMs{0};
    std::vector<StashItemRecord> unspawnedItems;
};

std::map<std::string, AccountStash> g_serverStashes; // account -> AccountStash
std::map<DWORD, std::string> g_machToAccount;        // mach/dpnid -> account

inline bool Readable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & ok) != 0;
}

inline std::string GetSafeModifierString(void* container, int offset) {
    if (!container || !Readable(container, offset + 4)) return "";
    void* ptr = *(void**)((char*)container + offset);
    if (!ptr) return "";

    auto isAsciiStr = [](const char* s) -> bool {
        if (!s || !Readable(s, 1)) return false;
        int len = 0;
        while (len < 64) {
            if (!Readable(s + len, 1)) return false;
            char c = s[len];
            if (c == '\0') return len > 0;
            if ((unsigned char)c < 32 || (unsigned char)c > 126) return false;
            len++;
        }
        return false;
    };

    // Format A: direct char* (where ptr itself is char*)
    if (isAsciiStr((char*)ptr)) {
        return std::string((char*)ptr);
    }

    // Format B: gpstring* (where *ptr is char* m_pData)
    if (Readable(ptr, 4)) {
        char* inner = *(char**)ptr;
        if (inner && isAsciiStr(inner)) {
            return std::string(inner);
        }
    }

    return "";
}

inline void* ResolveGo(uint32_t goid) {
    if (!goid || goid == *(uint32_t*)0x007ADC88) return nullptr;
    void* godb = *(void**)0x007ACF4C;
    return (godb && Readable(godb, 0x340)) ? s_findGo(godb, goid) : nullptr;
}

inline void* GetLocalScreenHero() {
    // 1. Try Server::GetScreenHero
    void* srv = *(void**)0x007ACF70;
    if (srv && Readable(srv, 0x20)) {
        typedef void* (__thiscall* GetScreenHeroFn)(void*);
        void* hero = ((GetScreenHeroFn)0x00570565)(srv);
        if (hero && Readable(hero, 0x80)) return hero;
    }
    // 2. Fallback to UIPartyManager selected party member
    void* uiPartyMgr = *(void**)0x007ACF90;
    if (uiPartyMgr && Readable(uiPartyMgr, 0x60)) {
        int idx = *(int*)((char*)uiPartyMgr + 0x50);
        if (idx >= 0 && idx < 8) {
            void* member = *(void**)((char*)uiPartyMgr + idx * 4);
            if (member && Readable(member, 0x90)) {
                uint32_t heroGoid = *(uint32_t*)((char*)member + 0x80);
                void* hero = ResolveGo(heroGoid);
                if (hero && Readable(hero, 0x80)) return hero;
            }
        }
    }
    return nullptr;
}

int GetInventoryDbLocation(void* inv, void* itemGo) {
    if (!inv || !itemGo || !Readable(inv, 0x80)) return 0x11;
    void* head = *(void**)((char*)inv + 0x68);
    if (!head || !Readable(head, 0x14)) return 0x11;
    void* node = head;
    s_mapFind((char*)inv + 0x64, &node, &itemGo);
    if (node && node != head && Readable(node, 0x14) && *(void**)((char*)node + 0x0C) == itemGo) {
        return *(int*)((char*)node + 0x10);
    }
    return 0x11;
}

void SetInventoryDbLocation(void* inv, void* itemGo, int loc) {
    if (!inv || !itemGo || !Readable(inv, 0x80)) return;
    void* head = *(void**)((char*)inv + 0x68);
    if (!head || !Readable(head, 0x14)) return;
    void* node = head;
    s_mapFind((char*)inv + 0x64, &node, &itemGo);
    if (node && node != head && Readable(node, 0x14) && *(void**)((char*)node + 0x0C) == itemGo) {
        *(int*)((char*)node + 0x10) = loc;
    }
}

void ZeroStashGoFootprint(void* heroInv, void* stashGo) {
    if (!stashGo || !Readable(stashGo, 0x80)) return;
    void* gui = *(void**)((char*)stashGo + 0x28);
    if (gui && Readable(gui, 0x30)) {
        *(int*)((char*)gui + 0x18) = 0; // m_InventoryWidth = 0
        *(int*)((char*)gui + 0x1C) = 0; // m_InventoryHeight = 0
    }
    if (heroInv && Readable(heroInv, 0x90)) {
        // Count how many actual visible backpacks the hero owns (excluding stashGo and any il_invalid/deleting backpacks)
        int realBackpacks = 0;
        void* ownerGo = *(void**)((char*)heroInv + 0x04);
        if (ownerGo && Readable(ownerGo, 0x90)) {
            void** b = *(void***)((char*)ownerGo + 0x88);
            void** e = *(void***)((char*)ownerGo + 0x8C);
            if (b && e && e >= b && (size_t)(e - b) <= 4096) {
                for (void** it = b; it != e; ++it) {
                    void* child = *it;
                    if (!child || child == stashGo || !Readable(child, 0x98)) continue;
                    if ((*((unsigned char*)child + 0x96) & 8) != 0) continue; // marked for deletion
                    void* childInv = *(void**)((char*)child + 0x2C);
                    if (childInv && Readable(childInv, 0x90) && *((unsigned char*)childInv + 0x86) != 0) {
                        int loc = GetInventoryDbLocation(heroInv, child);
                        if (loc == 0x11 /*il_invalid*/) {
                            void* cgui = *(void**)((char*)child + 0x28);
                            if (cgui && Readable(cgui, 0x30)) {
                                *(int*)((char*)cgui + 0x18) = 0;
                                *(int*)((char*)cgui + 0x1C) = 0;
                            }
                            continue;
                        }
                        realBackpacks++;
                    }
                }
            }
        }
        *(int*)((char*)heroInv + 0x88) = realBackpacks;
    }
}

void* FindHeroStashGo(void* inv) {
    if (!inv || !Readable(inv, 0x80)) return nullptr;
    void* heroGo = *(void**)((char*)inv + 0x04);
    if (!heroGo || !Readable(heroGo, 0x90)) return nullptr;
    uint32_t heroGoid = *(uint32_t*)((char*)heroGo + 0x4C);

    // 1. Check server stashes
    for (const auto& kv : g_serverStashes) {
        if (kv.second.heroGoid == heroGoid) {
            void* stashGo = ResolveGo(kv.second.stashGoid);
            if (stashGo && Readable(stashGo, 0x80)) return stashGo;
        }
    }

    // 2. Check client stash
    if (g_localStashGoid != 0) {
        void* stashGo = ResolveGo(g_localStashGoid);
        if (stashGo && Readable(stashGo, 0x80)) {
            void** b = *(void***)((char*)heroGo + 0x88);
            void** e = *(void***)((char*)heroGo + 0x8C);
            if (b && e && e > b && (size_t)(e - b) <= 4096) {
                for (void** it = b; it != e; ++it) {
                    if (*it == stashGo) return stashGo;
                }
            }
        }
    }

    // 3. Search hero's children for dsx_backpack_empty at il_invalid
    void** b = *(void***)((char*)heroGo + 0x88);
    void** e = *(void***)((char*)heroGo + 0x8C);
    if (b && e && e > b && (size_t)(e - b) <= 4096) {
        for (void** it = b; it != e; ++it) {
            void* child = *it;
            if (!child || !Readable(child, 0x98)) continue;
            if ((*((unsigned char*)child + 0x96) & 8) != 0) continue;
            const char* tmpl = s_templateName(child);
            if (tmpl && _stricmp(tmpl, "dsx_backpack_empty") == 0) {
                int loc = GetInventoryDbLocation(inv, child);
                if (loc == 0x11 /*il_invalid*/) {
                    return child;
                }
            }
        }
    }
    return nullptr;
}

float GetHeroUberLevel(void* hero) {
    if (!hero || !Readable(hero, 0x80)) return 0.0f;

    float uber = 0.0f;

    // Check GoActor skills belonging strictly to this specific hero (never shared across characters)
    void* actor = *(void**)((char*)hero + 0x00);
    if (actor && Readable(actor, 0x40)) {
        float m  = s_getSkillLevel(actor, "melee");
        float r  = s_getSkillLevel(actor, "ranged");
        float nm = s_getSkillLevel(actor, "nature magic");
        float cm = s_getSkillLevel(actor, "combat magic");
        float maxCombat = (std::max)({m, r, nm, cm});
        if (maxCombat > uber) uber = maxCombat;

        float u1 = s_getSkillLevel(actor, "uber");
        if (u1 > uber) uber = u1;
        float u2 = s_getSkillLevel(actor, "Uber");
        if (u2 > uber) uber = u2;

        char* sb = *(char**)((char*)actor + 0x18);
        char* se = *(char**)((char*)actor + 0x1C);
        if (sb && se && se >= sb && (size_t)(se - sb) <= 0x1000) {
            for (char* s = sb; s + 0x34 <= se; s += 0x48) {
                if (!Readable(s, 0x38)) break;
                const char* nm_str = *(char**)s;
                if (nm_str && Readable(nm_str, 4)) {
                    float lvl = *(float*)(s + 0x30);
                    if (lvl > uber && lvl < 1000.0f) {
                        if (_stricmp(nm_str, "Uber") == 0 || _stricmp(nm_str, "melee") == 0 ||
                            _stricmp(nm_str, "ranged") == 0 || _stricmp(nm_str, "nature magic") == 0 ||
                            _stricmp(nm_str, "combat magic") == 0) {
                            uber = lvl;
                        }
                    }
                }
            }
        }
    }

    return uber;
}

void PrintGameChat(const char* text) {
    if (!text || !*text) return;
    void* uiMp = *(void**)0x007ACF94;
    if (!uiMp || !Readable(uiMp, 0x20)) return;

    eng::GpStr narrow(text);
    struct GpWStr {
        wchar_t* p{nullptr};
        char alloc{0};
        ~GpWStr() {
            if (p) {
                typedef void(__thiscall* DtorFn)(void*);
                ((DtorFn)0x00402FD9)(this);
            }
        }
    } wide;

    typedef void*(__cdecl* NarrowToWideFn)(void* out, const void* in, int codePage);
    ((NarrowToWideFn)0x0041257B)(&wide, &narrow.s, 0);

    typedef void(__thiscall* ChatFn)(void* thisPtr, const void* str);
    ((ChatFn)0x0048D2AA)(uiMp, &wide);
}

static const wchar_t* GetEditBoxWide(void* box, int& outLen) {
    outLen = 0;
    if (!box) return L"";
    const wchar_t* p = *(const wchar_t**)((const char*)box + 0x1A8);
    if (!p) return L"";
    int len = *(const int*)((const char*)p - 10);
    if (len < 0) len = 0;
    outLen = len;
    return p;
}

std::string SanitizeFilename(const std::string& s) {
    std::string out;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out.push_back((char)tolower(c));
        }
    }
    return out.empty() ? "default" : out;
}

void SetStashUIText(const char* text) {
    void* w = eng::FindWindow("text_backpack", "DSX_backpack_ui");
    if (w && Readable(w, 0x1C0)) {
        eng::GpStr g(text);
        s_setText(w, &g.s, false);
    }
}

bool IsInventoryOpen() {
    void* uiPartyMgr = *(void**)0x007ACF90;
    if (!uiPartyMgr || !Readable(uiPartyMgr, 0x60)) return false;
    bool charVis = eng::InterfaceVisible("character");
    bool gridOpen = (*((char*)uiPartyMgr + 0x5F) != 0);
    return charVis && gridOpen;
}

void PlaySpellBookSfx() {
    if (Cfg().role == Role::Server) return;
    static DWORD s_lastPlayed = 0;
    DWORD now = GetTickCount();
    if (now - s_lastPlayed < 80) return;
    s_lastPlayed = now;

    void* soundMgr = *(void**)0x007ACFD8;
    if (!soundMgr || !Readable(soundMgr, 0x180)) return;
    eng::GpStr soundName("s_e_gui_spell_book");
    typedef int (__thiscall* PlaySampleFn)(void* mgr, const eng::gpstring* name, int p1, int p2, int p3);
    ((PlaySampleFn)0x0063AD8B)(soundMgr, &soundName.s, 0, 1, 0);
}

void OpenClientInventoryForStash(void* hero) {
    if (!hero || !Readable(hero, 0x80)) return;
    void* uiPartyMgr = *(void**)0x007ACF90;
    if (!uiPartyMgr || !Readable(uiPartyMgr, 0x1E8)) return;

    uint32_t heroGoid = *(uint32_t*)((char*)hero + 0x4C);
    int slotIdx = 0;
    void* partySlot = s_findPartyMember(uiPartyMgr, heroGoid, &slotIdx);
    if (!partySlot || !Readable(partySlot, 0x90)) {
        slotIdx = *(int*)((char*)uiPartyMgr + 0x50);
        if (slotIdx < 0 || slotIdx >= 8) slotIdx = 0;
        partySlot = *(void**)((char*)uiPartyMgr + slotIdx * 4);
    }

    if (partySlot && Readable(partySlot, 0x90)) {
        *((char*)partySlot + 0) = 1; // inventory grid open
        *((char*)partySlot + 3) = 1; // character paperdoll open
    }

    // 1. Open character + inventory panel if not already open
    if (!eng::InterfaceVisible("character")) {
        s_openCharInventory(uiPartyMgr, slotIdx, 1, 0);
    } else {
        // Ensure the inventory gridbox is expanded if only the paperdoll was open
        if (*((char*)uiPartyMgr + 0x5F) == 0) {
            s_showCharInventoryGrid(uiPartyMgr, 1, 0);
        }
    }

    // 2. Replace spellbook if open
    if (eng::InterfaceVisible("spell") || (partySlot && Readable(partySlot, 0x10) && *((char*)partySlot + 1) != 0)) {
        s_toggleSpellbook(uiPartyMgr, 0);
    }

    // 3. Replace regular backpack if open
    if (!g_sharedStashActive && eng::InterfaceVisible("DSX_backpack_ui")) {
        bool prev = g_suppressBackpackSfx;
        g_suppressBackpackSfx = true;
        s_toggleBackpack(uiPartyMgr, 0, 1);
        g_suppressBackpackSfx = prev;
    }
}

void OpenClientStashUI(void* hero, void* stashGo) {
    if (!hero || !stashGo || !Readable(hero, 0x80) || !Readable(stashGo, 0x80)) return;
    if (GetHeroUberLevel(hero) <= 30.0f) return;
    void* heroInv = *(void**)((char*)hero + 0x2C);
    void* uiPartyMgr = *(void**)0x007ACF90;
    if (!heroInv || !uiPartyMgr || !Readable(heroInv, 0xB0) || !Readable(uiPartyMgr, 0x1E8)) return;

    OpenClientInventoryForStash(hero);

    ZeroStashGoFootprint(heroInv, stashGo);
    SetInventoryDbLocation(heroInv, stashGo, 0x15 /*il_main*/);

    *(void**)((char*)heroInv + 0xA4) = stashGo;
    s_toggleBackpack(uiPartyMgr, 1, 0);
    g_suppressBackpackSfx = false;
    SetStashUIText("Shared Stash");

    g_sharedStashActive = true;
    g_wasInventoryOpen = true;
    g_pendingOpenStash = false;
    g_lastTitleMode = 1;
    g_lastTitleUpdate = GetTickCount();
    Log("SharedStash: opened stash UI on client (stashGoid=0x%08lX)", *(uint32_t*)((char*)stashGo + 0x4C));
}

void CleanupClientStashUI() {
    void* hero = GetLocalScreenHero();
    void* stashGo = ResolveGo(g_localStashGoid);
    void* uiPartyMgr = *(void**)0x007ACF90;

    if (hero && Readable(hero, 0x80)) {
        void* heroInv = *(void**)((char*)hero + 0x2C);
        if (heroInv && Readable(heroInv, 0xB0)) {
            if (stashGo) {
                SetInventoryDbLocation(heroInv, stashGo, 0x11 /*il_invalid*/);
                ZeroStashGoFootprint(heroInv, stashGo);
            }
            if (*(void**)((char*)heroInv + 0xA4) == stashGo) {
                *(void**)((char*)heroInv + 0xA4) = nullptr;
                if (uiPartyMgr && Readable(uiPartyMgr, 0x60)) {
                    int idx = *(int*)((char*)uiPartyMgr + 0x50);
                    if (idx >= 0 && idx < 8) {
                        void* partySlot = *(void**)((char*)uiPartyMgr + idx * 4);
                        if (partySlot && Readable(partySlot, 0x10)) {
                            *((char*)partySlot + 2) = 0;
                        }
                    }
                }
            }
        }
    }

    SetStashUIText("Backpack");
    g_lastTitleMode = 2;
    g_sharedStashActive = false;
    g_wasInventoryOpen = false;
}

void SendStashPacketToServer(uint32_t heroGoid, uint32_t type) {
    void* dpClient = g_dp8Client;
    if (!dpClient || !Readable(dpClient, 4)) {
        Log("SharedStash: SendStashPacketToServer failed (g_dp8Client is null)");
        return;
    }
    void** vtbl = *(void***)dpClient;
    if (!vtbl || !Readable(vtbl, 9 * sizeof(void*))) return;

    StashPacket pkt{};
    pkt.magic     = kStashMagic;
    pkt.type      = type;
    pkt.heroGoid  = heroGoid;
    pkt.stashGoid = g_localStashGoid;

    struct DpnBufferDesc {
        DWORD size;
        BYTE* data;
    } desc = { sizeof(pkt), (BYTE*)&pkt };

    DWORD asyncHandle = 0;
    typedef HRESULT(WINAPI* SendFn)(void*, const DpnBufferDesc*, DWORD, DWORD, void*, DWORD*, DWORD);
    HRESULT hr = ((SendFn)vtbl[8])(dpClient, &desc, 1, 0, nullptr, &asyncHandle, 0x08 /*DPNSEND_GUARANTEED*/);
    Log("SharedStash: SendStashPacketToServer hero=0x%08lX type=%u -> hr=0x%08lX", heroGoid, type, hr);
}

void SendStashPacketToClient(DWORD senderDpnid, uint32_t heroGoid, uint32_t stashGoid) {
    void* dpServer = g_dp8Server;
    if (!dpServer || !Readable(dpServer, 4)) {
        Log("SharedStash: SendStashPacketToClient failed (g_dp8Server is null)");
        return;
    }
    void** vtbl = *(void***)dpServer;
    if (!vtbl || !Readable(vtbl, 15 * sizeof(void*))) return;

    StashPacket pkt{};
    pkt.magic     = kStashMagic;
    pkt.type      = kPkt_StashInfo;
    pkt.heroGoid  = heroGoid;
    pkt.stashGoid = stashGoid;

    struct DpnBufferDesc {
        DWORD size;
        BYTE* data;
    } desc = { sizeof(pkt), (BYTE*)&pkt };

    DWORD asyncHandle = 0;
    typedef HRESULT(WINAPI* SendToFn)(void*, DWORD, const DpnBufferDesc*, DWORD, DWORD, void*, DWORD*, DWORD);
    HRESULT hr = ((SendToFn)vtbl[14])(dpServer, senderDpnid, &desc, 1, 0, nullptr, &asyncHandle, 0x08 /*DPNSEND_GUARANTEED*/);
    Log("SharedStash: SendStashPacketToClient dpnid=0x%08lX hero=0x%08lX stash=0x%08lX -> hr=0x%08lX",
        senderDpnid, heroGoid, stashGoid, hr);
}

void* CloneAndAddItemOnServer(void* ownerGo, void* inv, const char* tmplOrPContent, int loc, bool autoPlace) {
    if (!ownerGo || !inv || !tmplOrPContent || !*tmplOrPContent) return nullptr;
    void* godb = *(void**)0x007ACF4C;
    void* placement = *(void**)((char*)ownerGo + 0x40);
    if (!godb || !placement || !Readable(placement, 0x30)) return nullptr;

    const void* pos = s_getPosition(placement);
    if (!pos) return nullptr;

    void* req = s_makeCloneReq();
    if (!req) return nullptr;
    s_setStartingPos(req, pos);
    s_setForceClientAllowed(req, true);

    unsigned char ownerAllClients = (*((unsigned char*)ownerGo + 0x94) >> 1) & 1;
    unsigned char& reqByte30 = *((unsigned char*)req + 0x30);
    reqByte30 = (unsigned char)((reqByte30 & 0xEF) | (ownerAllClients << 4));

    uint32_t newGoid = 0;
    if (tmplOrPContent[0] == '#') {
        newGoid = s_sCloneGoWithContent(godb, req, tmplOrPContent, 0);
        if (!newGoid || newGoid == *(uint32_t*)0x007ADC88) {
            Log("SharedStash: SCloneGoWithContent failed for '%s', falling back to pure template", tmplOrPContent);
            std::string pureTmpl = tmplOrPContent + 1;
            size_t delim = pureTmpl.find_first_of(":/");
            if (delim != std::string::npos) {
                pureTmpl = pureTmpl.substr(0, delim);
            }
            newGoid = s_sCloneGo(godb, req, pureTmpl.c_str());
        }
    } else {
        newGoid = s_sCloneGo(godb, req, tmplOrPContent);
    }
    if (!newGoid || newGoid == *(uint32_t*)0x007ADC88) {
        Log("SharedStash: clone failed for '%s'", tmplOrPContent);
        return nullptr;
    }

    void* newGo = ResolveGo(newGoid);
    if (!newGo) {
        Log("SharedStash: failed to resolve goid 0x%08lX for '%s'", newGoid, tmplOrPContent);
        return nullptr;
    }

    s_rsAdd(inv, newGo, loc, 2 /*AO_COMMANDS*/, autoPlace);
    return newGo;
}

std::string GetStashFilePath(const std::string& account) {
    CreateDirectoryA("stash", nullptr);
    return "stash/" + SanitizeFilename(account) + ".gas";
}

// -------------------------------------------------------------------------------------------------
// Cross-server Stash Sync via Central Portal
// -------------------------------------------------------------------------------------------------
enum class SyncTaskType {
    Download,
    Upload
};

struct SyncTask {
    SyncTaskType type;
    std::string account;
    std::string data;
};

static CRITICAL_SECTION g_syncCs;
static bool g_syncCsInit = false;
static HANDLE g_syncEvent = NULL;
static HANDLE g_syncThread = NULL;
static volatile bool g_syncStop = false;
static std::vector<SyncTask> g_syncQueue;
static std::string g_activeSyncAccount;
static std::set<std::string> g_syncedAccounts;

static bool ParseUrl(const std::string& url, std::string& host, std::string& port) {
    if (url.empty()) return false;
    std::string s = url;
    if (s.rfind("http://", 0) == 0) {
        s = s.substr(7);
    }
    size_t slash = s.find('/');
    if (slash != std::string::npos) {
        s = s.substr(0, slash);
    }
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        port = s.substr(colon + 1);
    } else {
        host = s;
        port = "80";
    }
    return !host.empty();
}

static bool PortalHttpRequest(const char* method, const std::string& pathAndQuery, const std::string& body, std::string& outBody, int& outStatus) {
    outStatus = 0;
    outBody.clear();
    if (Cfg().portalHubUrl.empty() || Cfg().portalShardId.empty() || Cfg().portalShardKey.empty()) {
        return false;
    }
    std::string host, port;
    if (!ParseUrl(Cfg().portalHubUrl, host, port)) {
        return false;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        Log("SharedStash: getaddrinfo failed for %s:%s", host.c_str(), port.c_str());
        return false;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return false;
    }

    DWORD timeoutMs = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        Log("SharedStash: connect failed to %s:%s", host.c_str(), port.c_str());
        closesocket(s);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    std::string req = std::string(method) + " " + pathAndQuery + " HTTP/1.1\r\n"
                    + "Host: " + host + ":" + port + "\r\n"
                    + "X-Shard-Id: " + Cfg().portalShardId + "\r\n"
                    + "X-Shard-Key: " + Cfg().portalShardKey + "\r\n"
                    + "Connection: close\r\n";
    if (!body.empty() || strcmp(method, "PUT") == 0) {
        req += "Content-Type: text/plain; charset=utf-8\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    if (!body.empty()) {
        req += body;
    }

    int sent = send(s, req.data(), (int)req.size(), 0);
    if (sent <= 0) {
        closesocket(s);
        return false;
    }

    std::string resp;
    char buf[4096];
    int n = 0;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, n);
    }
    closesocket(s);

    if (resp.size() < 12) return false;
    size_t space = resp.find(' ');
    if (space == std::string::npos || space + 3 >= resp.size()) return false;
    outStatus = atoi(resp.c_str() + space + 1);

    size_t headerEnd = resp.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
        outBody = resp.substr(headerEnd + 4);
    }
    return true;
}

static bool SyncStashFromCentral(const std::string& account) {
    if (Cfg().role != Role::Server) return false;
    if (Cfg().portalHubUrl.empty()) return false;

    std::string pathAndQuery = "/api/shard/stash?account=" + SanitizeFilename(account);
    std::string outBody;
    int status = 0;
    bool ok = PortalHttpRequest("GET", pathAndQuery, "", outBody, status);
    if (!ok) {
        Log("SharedStash: failed to reach central portal to fetch stash for account '%s'", account.c_str());
        return false;
    }
    if (status == 200) {
        std::string filePath = GetStashFilePath(account);
        FILE* f = fopen(filePath.c_str(), "wb");
        if (f) {
            fwrite(outBody.data(), 1, outBody.size(), f);
            fclose(f);
            Log("SharedStash: downloaded latest stash for account '%s' (%zu bytes) from central portal",
                account.c_str(), outBody.size());
            return true;
        }
    } else if (status == 404) {
        Log("SharedStash: no stash on central portal for account '%s' (new/empty stash)", account.c_str());
        return true;
    } else {
        Log("SharedStash: central portal returned HTTP %d for account '%s'", status, account.c_str());
    }
    return false;
}

static bool UploadStashToCentral(const std::string& account, const std::string& data) {
    if (Cfg().role != Role::Server) return false;
    if (Cfg().portalHubUrl.empty()) return false;

    std::string pathAndQuery = "/api/shard/stash?account=" + SanitizeFilename(account);
    std::string outBody;
    int status = 0;
    bool ok = PortalHttpRequest("PUT", pathAndQuery, data, outBody, status);
    if (ok && status == 200) {
        Log("SharedStash: uploaded stash for account '%s' (%zu bytes) to central portal", account.c_str(), data.size());
        return true;
    } else {
        Log("SharedStash: failed to upload stash for account '%s' to central portal (status=%d, ok=%d)",
            account.c_str(), status, (int)ok);
        return false;
    }
}

static DWORD WINAPI StashSyncWorkerThread(LPVOID) {
    while (!g_syncStop) {
        WaitForSingleObject(g_syncEvent, 1000);
        if (g_syncStop) break;

        SyncTask task{};
        bool hasTask = false;

        EnterCriticalSection(&g_syncCs);
        if (!g_syncQueue.empty()) {
            task = g_syncQueue.front();
            g_syncQueue.erase(g_syncQueue.begin());
            g_activeSyncAccount = task.account;
            hasTask = true;
        }
        LeaveCriticalSection(&g_syncCs);

        if (hasTask) {
            if (task.type == SyncTaskType::Download) {
                SyncStashFromCentral(task.account);
                EnterCriticalSection(&g_syncCs);
                g_syncedAccounts.insert(task.account);
                LeaveCriticalSection(&g_syncCs);
            } else if (task.type == SyncTaskType::Upload) {
                UploadStashToCentral(task.account, task.data);
            }

            EnterCriticalSection(&g_syncCs);
            g_activeSyncAccount.clear();
            LeaveCriticalSection(&g_syncCs);
        }
    }
    return 0;
}

static void QueueStashDownload(const std::string& account) {
    if (Cfg().role != Role::Server || Cfg().portalHubUrl.empty()) return;
    EnterCriticalSection(&g_syncCs);
    bool exists = false;
    for (const auto& item : g_syncQueue) {
        if (item.account == account) {
            exists = true;
            break;
        }
    }
    if (!exists) {
        g_syncQueue.push_back({ SyncTaskType::Download, account, "" });
    }
    LeaveCriticalSection(&g_syncCs);
    SetEvent(g_syncEvent);
}

static void QueueStashUpload(const std::string& account, const std::string& content) {
    if (Cfg().role != Role::Server || Cfg().portalHubUrl.empty()) return;
    EnterCriticalSection(&g_syncCs);
    bool replaced = false;
    for (auto& item : g_syncQueue) {
        if (item.type == SyncTaskType::Upload && item.account == account) {
            item.data = content;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        g_syncQueue.push_back({ SyncTaskType::Upload, account, content });
    }
    LeaveCriticalSection(&g_syncCs);
    SetEvent(g_syncEvent);
}

static void FlushAccountUpload(const std::string& account, DWORD maxWaitMs) {
    if (Cfg().role != Role::Server || Cfg().portalHubUrl.empty()) return;
    SetEvent(g_syncEvent);
    DWORD start = GetTickCount();
    while (GetTickCount() - start < maxWaitMs) {
        bool pending = false;
        EnterCriticalSection(&g_syncCs);
        for (const auto& item : g_syncQueue) {
            if (item.type == SyncTaskType::Upload && item.account == account) {
                pending = true;
                break;
            }
        }
        if (g_activeSyncAccount == account) pending = true;
        LeaveCriticalSection(&g_syncCs);

        if (!pending) break;
        Sleep(20);
    }
}

uint32_t ComputeStashHash(void* stashGo) {
    if (!stashGo || !Readable(stashGo, 0x90)) return 0;
    void** b = *(void***)((char*)stashGo + 0x88);
    void** e = *(void***)((char*)stashGo + 0x8C);
    if (!b || !e || e < b || (size_t)(e - b) > 4096) return 0;

    uint32_t h = 0x811C9DC5;
    for (void** it = b; it != e; ++it) {
        void* item = *it;
        if (!item || !Readable(item, 0x90)) continue;
        uint32_t goid = *(uint32_t*)((char*)item + 0x4C);
        h = (h ^ goid) * 0x01000193;
        void* aspect = *(void**)((char*)item + 0x04);
        if (aspect && Readable(aspect, 0x60)) {
            uint32_t gv = (uint32_t)*(int*)((char*)aspect + 0x4C);
            h = (h ^ gv) * 0x01000193;
        }
        void** cb = *(void***)((char*)item + 0x88);
        void** ce = *(void***)((char*)item + 0x8C);
        if (cb && ce && ce > cb && (size_t)(ce - cb) <= 256) {
            for (void** cit = cb; cit != ce; ++cit) {
                void* child = *cit;
                if (!child || !Readable(child, 0x80)) continue;
                uint32_t cgoid = *(uint32_t*)((char*)child + 0x4C);
                h = (h ^ cgoid) * 0x01000193;
            }
        }
    }
    return h;
}

void SaveStashToFile(const std::string& account, void* stashGo) {
    if (account.empty() || !stashGo || !Readable(stashGo, 0x90)) return;
    std::string path = GetStashFilePath(account);
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        Log("SharedStash: failed to open %s for write", path.c_str());
        return;
    }

    fprintf(f, "[stash]\n{\n\taccount = \"%s\";\n\t[items]\n\t{\n", account.c_str());

    void** b = *(void***)((char*)stashGo + 0x88);
    void** e = *(void***)((char*)stashGo + 0x8C);
    int idx = 0;
    if (b && e && e > b && (size_t)(e - b) <= 4096) {
        for (void** it = b; it != e; ++it) {
            void* item = *it;
            if (!item || !Readable(item, 0x98)) continue;
            if ((*((unsigned char*)item + 0x96) & 8) != 0) continue;
            const char* tmpl = s_templateName(item);
            if (!tmpl || !*tmpl) continue;

            // Do not store gold (Go::m_pGold is at +0x24) or backpacks
            bool isGold = (*(void**)((char*)item + 0x24) != nullptr) || (_stricmp(tmpl, "gold") == 0);
            if (isGold) continue;
            void* bpInv = *(void**)((char*)item + 0x2C);
            if (bpInv && Readable(bpInv, 0x90) && *((unsigned char*)bpInv + 0x86) != 0) continue;

            void* gui = *(void**)((char*)item + 0x28);
            std::string varName = GetSafeModifierString(gui, 0x2C);

            int gv = 0;
            void* aspect = *(void**)((char*)item + 0x04);
            if (aspect && Readable(aspect, 0x60)) {
                gv = *(int*)((char*)aspect + 0x4C);
            }

            std::string pfx;
            std::string sfx;
            void* magic = *(void**)((char*)item + 0x30);
            if (magic && Readable(magic, 0x40)) {
                pfx = GetSafeModifierString(magic, 0x24);
                sfx = GetSafeModifierString(magic, 0x28);
            }

            fprintf(f, "\t\t[%d]\n\t\t{\n", idx++);
            fprintf(f, "\t\t\ttemplate_name = \"%s\";\n", tmpl);
            if (!varName.empty()) fprintf(f, "\t\t\tvariation_name = \"%s\";\n", varName.c_str());
            if (gv > 0) fprintf(f, "\t\t\tgold_value = %d;\n", gv);
            if (!pfx.empty()) fprintf(f, "\t\t\tmodifier_prefix_name = \"%s\";\n", pfx.c_str());
            if (!sfx.empty()) fprintf(f, "\t\t\tmodifier_suffix_name = \"%s\";\n", sfx.c_str());

            if (!pfx.empty() || !sfx.empty()) {
                Log("SharedStash: saved enchanted item '%s' pfx='%s' sfx='%s' var='%s'",
                    tmpl, pfx.c_str(), sfx.c_str(), varName.c_str());
            }

            // Persist any child spells if this item is a spellbook
            void** cb = *(void***)((char*)item + 0x88);
            void** ce = *(void***)((char*)item + 0x8C);
            void* itemInv = *(void**)((char*)item + 0x2C);
            if (cb && ce && ce > cb && (size_t)(ce - cb) <= 64 && itemInv && Readable(itemInv, 0x80)) {
                int sIdx = 0;
                for (void** cit = cb; cit != ce; ++cit) {
                    void* childSpell = *cit;
                    if (!childSpell || !Readable(childSpell, 0x80)) continue;
                    const char* sTmpl = s_templateName(childSpell);
                    if (!sTmpl || !*sTmpl) continue;
                    int sLoc = GetInventoryDbLocation(itemInv, childSpell);
                    fprintf(f, "\t\t\tspell_%d = \"%s:%d\";\n", sIdx++, sTmpl, sLoc);
                }
            }

            fprintf(f, "\t\t}\n");
        }
    }

    auto stIt = g_serverStashes.find(account);
    if (stIt != g_serverStashes.end()) {
        for (const auto& itemRec : stIt->second.unspawnedItems) {
            fprintf(f, "\t\t[%d]\n\t\t{\n", idx++);
            fprintf(f, "\t\t\ttemplate_name = \"%s\";\n", itemRec.template_name.c_str());
            if (!itemRec.variation_name.empty()) fprintf(f, "\t\t\tvariation_name = \"%s\";\n", itemRec.variation_name.c_str());
            if (itemRec.gold_value > 0) fprintf(f, "\t\t\tgold_value = %d;\n", itemRec.gold_value);
            if (!itemRec.modifier_prefix_name.empty()) fprintf(f, "\t\t\tmodifier_prefix_name = \"%s\";\n", itemRec.modifier_prefix_name.c_str());
            if (!itemRec.modifier_suffix_name.empty()) fprintf(f, "\t\t\tmodifier_suffix_name = \"%s\";\n", itemRec.modifier_suffix_name.c_str());
            int spIdx = 0;
            for (const auto& sp : itemRec.spells) {
                if (sp.second > 0) {
                    fprintf(f, "\t\t\tspell_%d = \"%s:%d\";\n", spIdx++, sp.first.c_str(), sp.second);
                } else {
                    fprintf(f, "\t\t\tspell_%d = \"%s\";\n", spIdx++, sp.first.c_str());
                }
            }
            fprintf(f, "\t\t}\n");
        }
    }

    fprintf(f, "\t}\n}\n");
    fclose(f);
    Log("SharedStash: saved %d item(s) to %s", idx, path.c_str());

    // Read back saved content and queue upload to central portal
    if (Cfg().role == Role::Server && !Cfg().portalHubUrl.empty()) {
        std::string content;
        FILE* rf = fopen(path.c_str(), "rb");
        if (rf) {
            fseek(rf, 0, SEEK_END);
            long sz = ftell(rf);
            fseek(rf, 0, SEEK_SET);
            if (sz > 0 && sz < 10 * 1024 * 1024) {
                content.resize(sz);
                fread(&content[0], 1, sz, rf);
            }
            fclose(rf);
        }
        if (!content.empty()) {
            QueueStashUpload(account, content);
        }
    }
}

void LoadStashFromFile(const std::string& account, void* hero, void* stashGo) {
    if (account.empty() || !hero || !stashGo || !Readable(stashGo, 0x90)) return;

    if (Cfg().role == Role::Server && !Cfg().portalHubUrl.empty()) {
        bool needWait = false;
        EnterCriticalSection(&g_syncCs);
        for (const auto& item : g_syncQueue) {
            if (item.type == SyncTaskType::Download && item.account == account) {
                needWait = true;
                break;
            }
        }
        if (!needWait && g_activeSyncAccount == account) needWait = true;
        LeaveCriticalSection(&g_syncCs);

        if (needWait) {
            DWORD start = GetTickCount();
            while (GetTickCount() - start < 1000) {
                Sleep(20);
                bool done = true;
                EnterCriticalSection(&g_syncCs);
                for (const auto& item : g_syncQueue) {
                    if (item.type == SyncTaskType::Download && item.account == account) {
                        done = false;
                        break;
                    }
                }
                if (g_activeSyncAccount == account) done = false;
                LeaveCriticalSection(&g_syncCs);
                if (done) break;
            }
        } else {
            bool synced = false;
            EnterCriticalSection(&g_syncCs);
            synced = (g_syncedAccounts.find(account) != g_syncedAccounts.end());
            LeaveCriticalSection(&g_syncCs);
            if (!synced) {
                SyncStashFromCentral(account);
                EnterCriticalSection(&g_syncCs);
                g_syncedAccounts.insert(account);
                LeaveCriticalSection(&g_syncCs);
            }
        }
    }

    std::string path = GetStashFilePath(account);
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        Log("SharedStash: no existing stash file at %s (starting empty)", path.c_str());
        return;
    }

    void* stashInv = *(void**)((char*)stashGo + 0x2C);
    if (!stashInv || !Readable(stashInv, 0xA0)) {
        fclose(f);
        return;
    }

    g_serverStashes[account].unspawnedItems.clear();

    char line[256];
    std::string currentTmpl, varName, pfx, sfx;
    std::vector<std::pair<std::string, int>> spells;
    int gv = 0;
    int loaded = 0;

    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "template_name", 13) == 0) {
            char* q = strchr(p, '"');
            if (q) { char* r = strchr(q + 1, '"'); if (r) { *r = '\0'; currentTmpl = q + 1; } }
        } else if (strncmp(p, "variation_name", 14) == 0) {
            char* q = strchr(p, '"');
            if (q) { char* r = strchr(q + 1, '"'); if (r) { *r = '\0'; varName = q + 1; } }
        } else if (strncmp(p, "gold_value", 10) == 0) {
            char* eq = strchr(p, '=');
            if (eq) gv = atoi(eq + 1);
        } else if (strncmp(p, "modifier_prefix_name", 20) == 0) {
            char* q = strchr(p, '"');
            if (q) { char* r = strchr(q + 1, '"'); if (r) { *r = '\0'; pfx = q + 1; } }
        } else if (strncmp(p, "modifier_suffix_name", 20) == 0) {
            char* q = strchr(p, '"');
            if (q) { char* r = strchr(q + 1, '"'); if (r) { *r = '\0'; sfx = q + 1; } }
        } else if (strncmp(p, "spell_", 6) == 0) {
            char* q = strchr(p, '"');
            if (q) {
                char* r = strchr(q + 1, '"');
                if (r) {
                    *r = '\0';
                    char* colon = strchr(q + 1, ':');
                    if (colon) {
                        *colon = '\0';
                        spells.push_back({ std::string(q + 1), atoi(colon + 1) });
                    } else {
                        spells.push_back({ std::string(q + 1), 0 });
                    }
                }
            }
        } else if (*p == '}' && !currentTmpl.empty()) {
            if (_stricmp(currentTmpl.c_str(), "gold") != 0) {
                std::string cloneStr;
                if (!varName.empty() || !pfx.empty() || !sfx.empty()) {
                    cloneStr = "#" + currentTmpl;
                    if (!varName.empty()) cloneStr += ":" + varName;
                    if (!pfx.empty()) cloneStr += "/+" + pfx;
                    if (!sfx.empty()) cloneStr += "/+" + sfx;
                } else {
                    cloneStr = currentTmpl;
                }

                if (!pfx.empty() || !sfx.empty() || !varName.empty()) {
                    Log("SharedStash: loading item '%s'", cloneStr.c_str());
                }

                void* newItem = CloneAndAddItemOnServer(hero, stashInv, cloneStr.c_str(), 0x15 /*il_main*/, true /*autoPlace*/);
                if (newItem && Readable(newItem, 0x80)) {
                    if (gv > 0) {
                        void* aspect = *(void**)((char*)newItem + 0x04);
                        if (aspect && Readable(aspect, 0x60)) {
                            s_setGoldValue(aspect, gv, 1);
                        }
                    }
                    if (!spells.empty()) {
                        void* itemInv = *(void**)((char*)newItem + 0x2C);
                        if (itemInv && Readable(itemInv, 0x80)) {
                            for (const auto& sp : spells) {
                                int loc = (sp.second >= 0 && sp.second < 12) ? sp.second : 0;
                                CloneAndAddItemOnServer(hero, itemInv, sp.first.c_str(), loc, false);
                            }
                        }
                    }
                    loaded++;
                } else {
                    Log("SharedStash: failed to spawn item '%s', preserving in unspawnedItems list", cloneStr.c_str());
                    auto& st = g_serverStashes[account];
                    st.unspawnedItems.push_back({ currentTmpl, varName, gv, pfx, sfx, spells });
                }
            }
            currentTmpl.clear();
            varName.clear();
            pfx.clear();
            sfx.clear();
            spells.clear();
            gv = 0;
        }
    }
    fclose(f);
    Log("SharedStash: loaded %d item(s) from %s", loaded, path.c_str());
}

void* GetOrCreateServerStash(const std::string& account, void* hero) {
    if (account.empty() || !hero || !Readable(hero, 0x80)) return nullptr;
    float uberLevel = GetHeroUberLevel(hero);
    if (uberLevel <= 30.0f) {
        Log("SharedStash: GetOrCreateServerStash denied for account '%s' hero 0x%08lX (uberLevel=%.1f <= 30.0)",
            account.c_str(), *(uint32_t*)((char*)hero + 0x4C), uberLevel);
        return nullptr;
    }
    uint32_t currentHeroGoid = *(uint32_t*)((char*)hero + 0x4C);
    void* godb = *(void**)0x007ACF4C;

    auto it = g_serverStashes.find(account);
    if (it != g_serverStashes.end()) {
        void* resolvedStash = ResolveGo(it->second.stashGoid);
        if (it->second.heroGoid == currentHeroGoid && resolvedStash && Readable(resolvedStash, 0x80)) {
            it->second.stashGo = resolvedStash;
            return resolvedStash;
        }
        // Different hero or invalid stashGo - persist and replace
        if (resolvedStash) {
            SaveStashToFile(account, resolvedStash);
            if (godb && Readable(godb, 0x340)) {
                s_sMarkGoAndChildrenForDeletion(godb, resolvedStash);
            }
        }
        g_serverStashes.erase(it);
    }

    void* heroInv = *(void**)((char*)hero + 0x2C);
    if (!heroInv || !Readable(heroInv, 0x90)) return nullptr;

    // Clean up any leftover il_invalid (0x11) dsx_backpack_empty imported from an old .dsparty save
    if (godb && Readable(godb, 0x340) && Readable(hero, 0x90)) {
        void** hb = *(void***)((char*)hero + 0x88);
        void** he = *(void***)((char*)hero + 0x8C);
        if (hb && he && he > hb && (size_t)(he - hb) <= 4096) {
            std::vector<void*> toDelete;
            for (void** hit = hb; hit != he; ++hit) {
                void* child = *hit;
                if (!child || !Readable(child, 0x98)) continue;
                if ((*((unsigned char*)child + 0x96) & 8) != 0) continue;
                const char* tmpl = s_templateName(child);
                if (tmpl && _stricmp(tmpl, "dsx_backpack_empty") == 0) {
                    int loc = GetInventoryDbLocation(heroInv, child);
                    if (loc == 0x11 /*il_invalid*/) {
                        toDelete.push_back(child);
                    }
                }
            }
            for (void* oldGhost : toDelete) {
                uint32_t oldGoid = *(uint32_t*)((char*)oldGhost + 0x4C);
                s_sMarkGoAndChildrenForDeletion(godb, oldGhost);
                Log("SharedStash: removed leftover il_invalid backpack 0x%08lX from hero 0x%08lX", oldGoid, currentHeroGoid);
            }
        }
    }

    // Clone and attach stashGo to heroInv at 0x11 (il_invalid) with autoPlace=false
    void* stashGo = CloneAndAddItemOnServer(hero, heroInv, "dsx_backpack_empty", 0x11 /*il_invalid*/, false /*autoPlace*/);
    if (!stashGo) {
        Log("SharedStash: failed to create stashGo for account '%s'", account.c_str());
        return nullptr;
    }

    ZeroStashGoFootprint(heroInv, stashGo);
    SetInventoryDbLocation(heroInv, stashGo, 0x11 /*il_invalid*/);

    uint32_t stashGoid = *(uint32_t*)((char*)stashGo + 0x4C);

    AccountStash st{};
    st.account     = account;
    st.heroGoid    = currentHeroGoid;
    st.stashGoid   = stashGoid;
    st.stashGo     = stashGo;
    st.lastSavedMs = GetTickCount();

    // Populate with saved items
    LoadStashFromFile(account, hero, stashGo);

    // Ensure dedicated server keeps stashInv->m_pGridbox null so client UIGridbox is authoritative
    if (Cfg().role == Role::Server) {
        void* stashInv = *(void**)((char*)stashGo + 0x2C);
        if (stashInv && Readable(stashInv, 0xA0)) {
            *(void**)((char*)stashInv + 0x98) = nullptr;
        }
    }

    // Initial item count & hash
    void** ib = *(void***)((char*)stashGo + 0x88);
    void** ie = *(void***)((char*)stashGo + 0x8C);
    st.lastItemCount = (ib && ie && ie >= ib) ? (int)(ie - ib) : 0;
    st.lastHash      = ComputeStashHash(stashGo);

    g_serverStashes[account] = st;
    Log("SharedStash: initialized server stash (goid 0x%08lX) for account '%s' (hero 0x%08lX)",
        stashGoid, account.c_str(), currentHeroGoid);

    return stashGo;
}

} // namespace

// -------------------------------------------------------------------------------------------------
// Prevent stashGo (or any il_invalid/zero-footprint dsx_backpack_empty) from being saved to .dsparty
// -------------------------------------------------------------------------------------------------
extern "C" char __fastcall Orig_GoInventory_XferEntry(void* inv, void* unused_edx, void* persist, void* node);
__asm__(
    ".text\n"
    ".globl _Orig_GoInventory_XferEntry\n"
    ".globl @Orig_GoInventory_XferEntry@16\n"
    "_Orig_GoInventory_XferEntry:\n"
    "@Orig_GoInventory_XferEntry@16:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $0x60, %esp\n"
    "  movl $0x0057A778, %eax\n"
    "  jmp *%eax\n"
);

extern "C" char __fastcall Hook_GoInventory_XferEntry(void* inv, void* unused_edx, void* persist, void* node) {
    if (node && Readable(node, 0x14)) {
        void* itemGo = *(void**)((char*)node + 0x0C);
        int loc = *(int*)((char*)node + 0x10);
        if (itemGo && Readable(itemGo, 0x80)) {
            uint32_t goid = *(uint32_t*)((char*)itemGo + 0x4C);
            if (g_localStashGoid != 0 && goid == g_localStashGoid) {
                return 1;
            }
            const char* tmpl = s_templateName(itemGo);
            if (tmpl && _stricmp(tmpl, "dsx_backpack_empty") == 0) {
                void* gui = *(void**)((char*)itemGo + 0x28);
                int invW = (gui && Readable(gui, 0x30)) ? *(int*)((char*)gui + 0x18) : -1;
                if (loc == 0x11 /*il_invalid*/ || invW == 0) {
                    return 1;
                }
            }
        }
    }
    return Orig_GoInventory_XferEntry(inv, unused_edx, persist, node);
}

// -------------------------------------------------------------------------------------------------
// Spellbook SFX for Opening/Closing Backpack and Shared Stash (only while inventory is already open)
// -------------------------------------------------------------------------------------------------
extern "C" void __fastcall Orig_UIPartyManager_ToggleBackpack(void* uiPartyMgr, void* unused_edx, int open, char unk);
__asm__(
    ".text\n"
    ".globl _Orig_UIPartyManager_ToggleBackpack\n"
    ".globl @Orig_UIPartyManager_ToggleBackpack@16\n"
    "_Orig_UIPartyManager_ToggleBackpack:\n"
    "@Orig_UIPartyManager_ToggleBackpack@16:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $0x20, %esp\n"
    "  movl $0x0049D0FC, %eax\n"
    "  jmp *%eax\n"
);

extern "C" void __fastcall Hook_UIPartyManager_ToggleBackpack(void* uiPartyMgr, void* unused_edx, int open, char unk) {
    void* retAddr = __builtin_return_address(0);
    bool wasInvOpen = IsInventoryOpen();
    bool suppress = g_suppressBackpackSfx || 
                    (open == 0 && unk == 0) || 
                    (retAddr == (void*)0x0049CC17);

    Orig_UIPartyManager_ToggleBackpack(uiPartyMgr, unused_edx, open, unk);

    if (wasInvOpen && !suppress && IsInventoryOpen()) {
        PlaySpellBookSfx();
    }
}

// -------------------------------------------------------------------------------------------------
// Unique & Rare Item Color Fix (preserve gold/yellow text for pcontent_special_type)
// -------------------------------------------------------------------------------------------------
extern "C" const eng::gpstring* __fastcall Orig_GoGui_GetToolTipColorName(void* goGui);
__asm__(
    ".text\n"
    ".globl _Orig_GoGui_GetToolTipColorName\n"
    ".globl @Orig_GoGui_GetToolTipColorName@4\n"
    "_Orig_GoGui_GetToolTipColorName:\n"
    "@Orig_GoGui_GetToolTipColorName@4:\n"
    "  testb $1, 0x007AF400\n"
    "  pushl $0x00541E5F\n"
    "  ret\n"
);

extern "C" const eng::gpstring* __fastcall Hook_GoGui_GetToolTipColorName(void* goGui, void* unused_edx) {
    const eng::gpstring* res = Orig_GoGui_GetToolTipColorName(goGui);
    if (res && res->p && *res->p) {
        return res;
    }
    if (goGui && Readable(goGui, 8)) {
        void* go = *(void**)((char*)goGui + 4);
        if (go && Readable(go, 0x80)) {
            typedef const eng::gpstring* (__thiscall* GetComponentStringFn)(void* go, const char* comp, const char* prop);
            static const auto s_getCompStr = (GetComponentStringFn)0x0047C0F9;
            const eng::gpstring* special = s_getCompStr(go, "common", "pcontent_special_type");
            if (special && special->p && *special->p) {
                return special;
            }
        }
    }
    return res;
}

// -------------------------------------------------------------------------------------------------
// Backpack in Shared Stash rejection message fix ("inside Shared Stash" instead of "inside another backpack")
// -------------------------------------------------------------------------------------------------
static const char* kMsgBpInBp    = "$MSG$You may not place a backpack inside another backpack.\n";
static const char* kMsgBpInStash = "$MSG$You may not place a backpack inside Shared Stash.\n";

extern "C" const char* __cdecl DetermineBackpackMsg_Drop(void* param2, void* param3) {
    if (g_sharedStashActive && g_localStashGoid != 0) {
        if (param2 && Readable(param2, 0x50)) {
            uint32_t goid2 = *(uint32_t*)((char*)param2 + 0x4C);
            if (goid2 == g_localStashGoid) return kMsgBpInStash;
        }
        if (param3 && Readable(param3, 0x50)) {
            uint32_t goid3 = *(uint32_t*)((char*)param3 + 0x4C);
            if (goid3 == g_localStashGoid) return kMsgBpInStash;
        }
        void* uiPartyMgr = *(void**)0x007ACF90;
        if (uiPartyMgr && Readable(uiPartyMgr, 0x1E8)) {
            uint32_t activeBpGoid = *(uint32_t*)((char*)uiPartyMgr + 0x1E0);
            if (activeBpGoid == g_localStashGoid) return kMsgBpInStash;
        }
    }
    return kMsgBpInBp;
}

extern "C" const char* __cdecl DetermineBackpackMsg_Hover() {
    if (g_sharedStashActive && g_localStashGoid != 0) {
        void* uiPartyMgr = *(void**)0x007ACF90;
        if (uiPartyMgr && Readable(uiPartyMgr, 0x1E8)) {
            uint32_t activeBpGoid = *(uint32_t*)((char*)uiPartyMgr + 0x1E0);
            if (activeBpGoid == g_localStashGoid) return kMsgBpInStash;
        }
    }
    return kMsgBpInBp;
}

extern "C" void Hook_PushBackpackMsg_Drop();
__asm__(
    ".text\n"
    ".globl _Hook_PushBackpackMsg_Drop\n"
    "_Hook_PushBackpackMsg_Drop:\n"
    "  pushl 12(%ebp)\n"
    "  pushl 8(%ebp)\n"
    "  call _DetermineBackpackMsg_Drop\n"
    "  addl $8, %esp\n"
    "  pushl %eax\n"
    "  movl $0x00500900, %edx\n"
    "  jmp *%edx\n"
);

extern "C" void Hook_PushBackpackMsg_Hover();
__asm__(
    ".text\n"
    ".globl _Hook_PushBackpackMsg_Hover\n"
    "_Hook_PushBackpackMsg_Hover:\n"
    "  call _DetermineBackpackMsg_Hover\n"
    "  pushl %eax\n"
    "  movl $0x004FF318, %edx\n"
    "  jmp *%edx\n"
);

// -------------------------------------------------------------------------------------------------
// Fix Ctrl+Click Drop & Drop From Shared Stash
// -------------------------------------------------------------------------------------------------
extern "C" bool __fastcall Orig_GoInventory_ContainsIncludeBackpacks(void* inv, void* unused_edx, void* itemGo);
__asm__(
    ".text\n"
    ".globl _Orig_GoInventory_ContainsIncludeBackpacks\n"
    ".globl @Orig_GoInventory_ContainsIncludeBackpacks@12\n"
    "_Orig_GoInventory_ContainsIncludeBackpacks:\n"
    "@Orig_GoInventory_ContainsIncludeBackpacks@12:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  pushl %ecx\n"
    "  pushl %ecx\n"
    "  pushl %ebx\n"
    "  movl $0x00573762, %eax\n"
    "  jmp *%eax\n"
);

extern "C" bool __fastcall Hook_GoInventory_ContainsIncludeBackpacks(void* inv, void* unused_edx, void* itemGo) {
    if (Orig_GoInventory_ContainsIncludeBackpacks(inv, unused_edx, itemGo)) {
        return true;
    }
    void* stashGo = FindHeroStashGo(inv);
    if (stashGo) {
        void* stashInv = *(void**)((char*)stashGo + 0x2C);
        if (stashInv && Readable(stashInv, 0x80)) {
            typedef bool (__thiscall* ContainsFn)(void*, void*);
            static const auto s_contains = (ContainsFn)0x00573731;
            if (s_contains(stashInv, itemGo)) {
                return true;
            }
        }
    }
    return false;
}

extern "C" bool __fastcall Orig_GoInventory_RemoveFromDbAndPlaceInWorld(void* inv, void* unused_edx, void* itemGo, void* p2, void* p3, void* p4);
__asm__(
    ".text\n"
    ".globl _Orig_GoInventory_RemoveFromDbAndPlaceInWorld\n"
    ".globl @Orig_GoInventory_RemoveFromDbAndPlaceInWorld@24\n"
    "_Orig_GoInventory_RemoveFromDbAndPlaceInWorld:\n"
    "@Orig_GoInventory_RemoveFromDbAndPlaceInWorld@24:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $0x3C, %esp\n"
    "  movl $0x005764F2, %eax\n"
    "  jmp *%eax\n"
);

extern "C" bool __fastcall Hook_GoInventory_RemoveFromDbAndPlaceInWorld(void* inv, void* unused_edx, void* itemGo, void* p2, void* p3, void* p4) {
    if (Orig_GoInventory_RemoveFromDbAndPlaceInWorld(inv, unused_edx, itemGo, p2, p3, p4)) {
        return true;
    }
    void* stashGo = FindHeroStashGo(inv);
    if (stashGo) {
        void* stashInv = *(void**)((char*)stashGo + 0x2C);
        if (stashInv && Readable(stashInv, 0x80)) {
            bool ok = Orig_GoInventory_RemoveFromDbAndPlaceInWorld(stashInv, unused_edx, itemGo, p2, p3, p4);
            if (ok) {
                void* heroGo = *(void**)((char*)inv + 0x04);
                if (heroGo && Readable(heroGo, 0x80)) {
                    uint32_t heroGoid = *(uint32_t*)((char*)heroGo + 0x4C);
                    for (auto& kv : g_serverStashes) {
                        if (kv.second.heroGoid == heroGoid) {
                            kv.second.lastHash = ComputeStashHash(stashGo);
                            void** b = *(void***)((char*)stashGo + 0x88);
                            void** e = *(void***)((char*)stashGo + 0x8C);
                            kv.second.lastItemCount = (b && e && e >= b) ? (int)(e - b) : 0;
                            SaveStashToFile(kv.second.account, stashGo);
                            kv.second.lastSavedMs = GetTickCount();
                            break;
                        }
                    }
                }
                return true;
            }
        }
    }
    return false;
}

void SharedStash_SetDirectPlay(void* dpClient, void* dpServer) {
    if (dpClient) g_dp8Client = dpClient;
    if (dpServer) g_dp8Server = dpServer;
}

void SharedStash_Init() {
    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // Initialize sync worker
    if (!g_syncCsInit) {
        InitializeCriticalSection(&g_syncCs);
        g_syncCsInit = true;
    }
    if (!g_syncEvent) {
        g_syncEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    }
    if (!g_syncThread && Cfg().role == Role::Server) {
        g_syncStop = false;
        g_syncThread = CreateThread(NULL, 0, StashSyncWorkerThread, NULL, 0, NULL);
    }

    if (!g_stashPktCsInit) {
        InitializeCriticalSection(&g_stashPktCs);
        g_stashPktCsInit = true;
    }
    g_sharedStashActive = false;
    g_wasInventoryOpen = false;
    g_pendingOpenStash = false;
    g_eatNextReturnChar = false;
    g_suppressBackpackSfx = false;
    g_localStashGoid = 0;
    g_lastTitleMode = 0;
    g_lastTitleUpdate = 0;

    // Hook GoInventory::XferInventoryDbEntry at 0x0057A772 (6 bytes: 55 8B EC 83 EC 60)
    const BYTE kXferExpect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x60 };
    BYTE* pXfer = (BYTE*)0x0057A772;
    if (Readable(pXfer, 6) && memcmp(pXfer, kXferExpect, 6) == 0) {
        DWORD oldProt = 0;
        if (VirtualProtect(pXfer, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pXfer[0] = 0xE9;
            *(int32_t*)(pXfer + 1) = (int32_t)((DWORD)(void*)&Hook_GoInventory_XferEntry - (0x0057A772 + 5));
            pXfer[5] = 0x90;
            VirtualProtect(pXfer, 6, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), pXfer, 6);
            Log("SharedStash: hooked GoInventory::XferEntry at 0x0057A772 (excluding stashGo from .dsparty saves)");
        }
    }

    // Hook UIPartyManager::ToggleBackpack at 0x0049D0F6 (6 bytes: 55 8B EC 83 EC 20)
    const BYTE kToggleBpExpect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20 };
    BYTE* pToggleBp = (BYTE*)0x0049D0F6;
    if (Readable(pToggleBp, 6) && memcmp(pToggleBp, kToggleBpExpect, 6) == 0) {
        DWORD oldProt = 0;
        if (VirtualProtect(pToggleBp, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pToggleBp[0] = 0xE9;
            *(int32_t*)(pToggleBp + 1) = (int32_t)((DWORD)(void*)&Hook_UIPartyManager_ToggleBackpack - (0x0049D0F6 + 5));
            pToggleBp[5] = 0x90;
            VirtualProtect(pToggleBp, 6, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), pToggleBp, 6);
            Log("SharedStash: hooked UIPartyManager::ToggleBackpack at 0x0049D0F6 (spellbook SFX for backpack/stash)");
        }
    }

    // Hook GoGui::GetToolTipColorName at 0x00541E58 (7 bytes: F6 05 00 F4 7A 00 01)
    const BYTE kColorNameExpect[7] = { 0xF6, 0x05, 0x00, 0xF4, 0x7A, 0x00, 0x01 };
    BYTE* pColorName = (BYTE*)0x00541E58;
    if (Readable(pColorName, 7) && memcmp(pColorName, kColorNameExpect, 7) == 0) {
        DWORD oldProt = 0;
        if (VirtualProtect(pColorName, 7, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pColorName[0] = 0xE9;
            *(int32_t*)(pColorName + 1) = (int32_t)((DWORD)(void*)&Hook_GoGui_GetToolTipColorName - (0x00541E58 + 5));
            pColorName[5] = 0x90;
            pColorName[6] = 0x90;
            VirtualProtect(pColorName, 7, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), pColorName, 7);
            Log("SharedStash: hooked GoGui::GetToolTipColorName at 0x00541E58 (unique/rare item color fix)");
        }
    }

    // Hook backpack rejection message (hover) at 0x004FF313 (5 bytes: 68 B0 B7 78 00)
    const BYTE kPushMsgExpect[5] = { 0x68, 0xB0, 0xB7, 0x78, 0x00 };
    BYTE* pMsg1 = (BYTE*)0x004FF313;
    if (Readable(pMsg1, 5) && memcmp(pMsg1, kPushMsgExpect, 5) == 0) {
        DWORD oldProt = 0;
        if (VirtualProtect(pMsg1, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pMsg1[0] = 0xE9;
            *(int32_t*)(pMsg1 + 1) = (int32_t)((DWORD)(void*)&Hook_PushBackpackMsg_Hover - (0x004FF313 + 5));
            VirtualProtect(pMsg1, 5, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), pMsg1, 5);
            Log("SharedStash: hooked backpack rejection message (hover) at 0x004FF313");
        }
    }

    // Hook backpack rejection message (drop) at 0x005008FB (5 bytes: 68 B0 B7 78 00)
    BYTE* pMsg2 = (BYTE*)0x005008FB;
    if (Readable(pMsg2, 5) && memcmp(pMsg2, kPushMsgExpect, 5) == 0) {
        DWORD oldProt = 0;
        if (VirtualProtect(pMsg2, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pMsg2[0] = 0xE9;
            *(int32_t*)(pMsg2 + 1) = (int32_t)((DWORD)(void*)&Hook_PushBackpackMsg_Drop - (0x005008FB + 5));
            VirtualProtect(pMsg2, 5, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), pMsg2, 5);
            Log("SharedStash: hooked backpack rejection message (drop) at 0x005008FB");
        }
    }

    // Hook GoInventory::ContainsIncludeBackpacks at 0x0057375C (6 bytes: 55 8B EC 51 51 53)
    const BYTE kContainsExpect[6] = { 0x55, 0x8B, 0xEC, 0x51, 0x51, 0x53 };
    BYTE* pContains = (BYTE*)0x0057375C;
    if (Readable(pContains, 6) && memcmp(pContains, kContainsExpect, 6) == 0) {
        DWORD oldProt = 0;
        if (VirtualProtect(pContains, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pContains[0] = 0xE9;
            *(int32_t*)(pContains + 1) = (int32_t)((DWORD)(void*)&Hook_GoInventory_ContainsIncludeBackpacks - (0x0057375C + 5));
            pContains[5] = 0x90;
            VirtualProtect(pContains, 6, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), pContains, 6);
            Log("SharedStash: hooked GoInventory::ContainsIncludeBackpacks at 0x0057375C (drop fix)");
        }
    }

    // Hook GoInventory::RemoveFromDbAndPlaceInWorld at 0x005764EC (6 bytes: 55 8B EC 83 EC 3C)
    const BYTE kRemoveExpect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x3C };
    BYTE* pRemove = (BYTE*)0x005764EC;
    if (Readable(pRemove, 6) && memcmp(pRemove, kRemoveExpect, 6) == 0) {
        DWORD oldProt = 0;
        if (VirtualProtect(pRemove, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
            pRemove[0] = 0xE9;
            *(int32_t*)(pRemove + 1) = (int32_t)((DWORD)(void*)&Hook_GoInventory_RemoveFromDbAndPlaceInWorld - (0x005764EC + 5));
            pRemove[5] = 0x90;
            VirtualProtect(pRemove, 6, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), pRemove, 6);
            Log("SharedStash: hooked GoInventory::RemoveFromDbAndPlaceInWorld at 0x005764EC (drop fix)");
        }
    }

    Log("SharedStash: module initialized.");
}

void SharedStash_OnPlayerJoin(DWORD mach, const char* account) {
    if (account && *account) {
        g_machToAccount[mach] = account;
        Log("SharedStash: mapped mach 0x%08lX to account '%s'", mach, account);
        QueueStashDownload(account);
    }
}

void SharedStash_OnPlayerLeave(DWORD mach) {
    auto it = g_machToAccount.find(mach);
    if (it != g_machToAccount.end()) {
        std::string account = it->second;
        g_machToAccount.erase(it);
        auto stIt = g_serverStashes.find(account);
        if (stIt != g_serverStashes.end()) {
            void* stashGo = ResolveGo(stIt->second.stashGoid);
            if (stashGo) {
                SaveStashToFile(account, stashGo);
            }
            g_serverStashes.erase(stIt);
            Log("SharedStash: saved and cleaned up stash for account '%s'", account.c_str());
        }
        FlushAccountUpload(account, 1000);
    }
}

bool SharedStash_OnReceivePacket(bool isServer, DWORD senderDpnid, const BYTE* data, DWORD size) {
    if (!data || size < sizeof(StashPacket)) return false;
    const auto* pkt = (const StashPacket*)data;
    if (pkt->magic != kStashMagic) return false;

    if (g_stashPktCsInit) {
        EnterCriticalSection(&g_stashPktCs);
        int nextTail = (g_stashPktTail + 1) % 32;
        if (nextTail != g_stashPktHead) {
            g_stashPktQueue[g_stashPktTail] = { isServer, senderDpnid, *pkt };
            g_stashPktTail = nextTail;
        }
        LeaveCriticalSection(&g_stashPktCs);
    }
    return true;
}

void SharedStash_Tick() {
    DWORD now = GetTickCount();

    // 1. Process queued DirectPlay packets on the main game thread
    if (g_stashPktCsInit) {
        QueuedStashPacket localPkts[16];
        int pktCount = 0;
        EnterCriticalSection(&g_stashPktCs);
        while (g_stashPktHead != g_stashPktTail && pktCount < 16) {
            localPkts[pktCount++] = g_stashPktQueue[g_stashPktHead];
            g_stashPktHead = (g_stashPktHead + 1) % 32;
        }
        LeaveCriticalSection(&g_stashPktCs);

        for (int i = 0; i < pktCount; ++i) {
            const QueuedStashPacket& q = localPkts[i];
            if (q.isServer) {
                if (q.pkt.type == kPkt_ReqOpen) {
                    std::string account;
                    auto it = g_machToAccount.find(q.senderDpnid);
                    if (it != g_machToAccount.end()) {
                        account = it->second;
                    } else {
                        account = JG_GetAccountForDpnid(q.senderDpnid);
                        if (account.empty()) account = "player";
                        g_machToAccount[q.senderDpnid] = account;
                    }

                    Log("SharedStash: server processing ReqOpen from dpnid 0x%08lX account '%s' hero 0x%08lX",
                        q.senderDpnid, account.c_str(), q.pkt.heroGoid);

                    void* hero = ResolveGo(q.pkt.heroGoid);
                    if (hero) {
                        float uberLevel = GetHeroUberLevel(hero);
                        if (uberLevel <= 30.0f) {
                            Log("SharedStash: server REJECTED ReqOpen from dpnid 0x%08lX account '%s' hero 0x%08lX (uberLevel=%.1f <= 30.0)",
                                q.senderDpnid, account.c_str(), q.pkt.heroGoid, uberLevel);
                            continue;
                        }
                        void* stashGo = GetOrCreateServerStash(account, hero);
                        if (stashGo) {
                            uint32_t stashGoid = *(uint32_t*)((char*)stashGo + 0x4C);
                            SendStashPacketToClient(q.senderDpnid, q.pkt.heroGoid, stashGoid);
                        }
                    } else {
                        Log("SharedStash: server could not resolve hero 0x%08lX", q.pkt.heroGoid);
                    }
                }
            } else {
                if (q.pkt.type == kPkt_StashInfo) {
                    g_localStashGoid = q.pkt.stashGoid;
                    Log("SharedStash: client received stashGoid 0x%08lX from server", g_localStashGoid);

                    void* hero = GetLocalScreenHero();
                    if (hero) {
                        float uber = GetHeroUberLevel(hero);
                        if (uber <= 30.0f) {
                            Log("SharedStash: client rejected StashInfo (hero uberLevel=%.1f <= 30.0)", uber);
                            g_localStashGoid = 0;
                            g_pendingOpenStash = false;
                            continue;
                        }
                    }
                    void* stashGo = ResolveGo(g_localStashGoid);
                    if (hero && stashGo) {
                        OpenClientStashUI(hero, stashGo);
                    } else {
                        Log("SharedStash: stashGo 0x%08lX not resolved yet on client, pending open", g_localStashGoid);
                        g_pendingOpenStash = true;
                    }
                }
            }
        }
    }

    // 2. Client tick: complete pending open and manage stash / backpack UI state
    if (Cfg().role != Role::Server) {
        void* curHero = GetLocalScreenHero();
        if (curHero && Readable(curHero, 0x50)) {
            uint32_t curHeroGoid = *(uint32_t*)((char*)curHero + 0x4C);
            static uint32_t s_lastClientHeroGoid = 0;
            if (s_lastClientHeroGoid != 0 && curHeroGoid != s_lastClientHeroGoid) {
                // Character switched! Invalidate all client stash state
                CleanupClientStashUI();
                g_localStashGoid = 0;
                g_pendingOpenStash = false;
            }
            s_lastClientHeroGoid = curHeroGoid;
        }

        if (g_pendingOpenStash && g_localStashGoid != 0) {
            void* hero = GetLocalScreenHero();
            void* stashGo = ResolveGo(g_localStashGoid);
            if (hero && stashGo) {
                float uber = GetHeroUberLevel(hero);
                if (uber > 30.0f) {
                    OpenClientStashUI(hero, stashGo);
                } else {
                    g_pendingOpenStash = false;
                    g_localStashGoid = 0;
                }
            }
        }

        void* uiPartyMgr = *(void**)0x007ACF90;
        uint32_t invalidGoid = *(uint32_t*)0x007ADC88;
        uint32_t activeBpGoid = (uiPartyMgr && Readable(uiPartyMgr, 0x1E8))
            ? *(uint32_t*)((char*)uiPartyMgr + 0x1E0)
            : invalidGoid;

        bool bpVis = eng::InterfaceVisible("DSX_backpack_ui");
        bool charVis = eng::InterfaceVisible("character");
        bool spellVis = eng::InterfaceVisible("spell");

        if (g_sharedStashActive) {
            void* hero = GetLocalScreenHero();
            void* stashGo = ResolveGo(g_localStashGoid);
            if (!hero || !stashGo || !uiPartyMgr || !Readable(hero, 0x80) || !Readable(uiPartyMgr, 0x1E8)) {
                CleanupClientStashUI();
            } else {
                void* heroInv = *(void**)((char*)hero + 0x2C);
                void* selectedBpGo = (heroInv && Readable(heroInv, 0xB0)) ? *(void**)((char*)heroInv + 0xA4) : nullptr;
                int slotIdx = *(int*)((char*)uiPartyMgr + 0x50);
                void* partySlot = (slotIdx >= 0 && slotIdx < 8) ? *(void**)((char*)uiPartyMgr + slotIdx * 4) : nullptr;
                char spellbookFlag = (partySlot && Readable(partySlot, 0x10)) ? *((char*)partySlot + 1) : 0;
                char backpackFlag  = (partySlot && Readable(partySlot, 0x10)) ? *((char*)partySlot + 2) : 0;
                bool mainGridExpanded = (*((char*)uiPartyMgr + 0x5F) != 0);
                bool invOpen = charVis && mainGridExpanded;

                // Did the player replace the Shared Stash with the Spell Book or a regular Backpack, or close it?
                bool replaced = (selectedBpGo != stashGo) ||
                                (backpackFlag == 0) ||
                                spellVis ||
                                (spellbookFlag != 0) ||
                                (bpVis && activeBpGoid != 0 && activeBpGoid != invalidGoid && activeBpGoid != g_localStashGoid);

                if (replaced) {
                    CleanupClientStashUI();
                } else if (!invOpen) {
                    // Inventory was closed (via 'I' hotkey or inventory button), but Shared Stash was not closed via X
                    g_wasInventoryOpen = false;
                    if (bpVis) {
                        g_suppressBackpackSfx = true;
                        s_toggleBackpack(uiPartyMgr, 0, 0);
                        g_suppressBackpackSfx = false;
                        bpVis = false;
                    }
                    if (heroInv && Readable(heroInv, 0xB0)) {
                        SetInventoryDbLocation(heroInv, stashGo, 0x11 /*il_invalid*/);
                        ZeroStashGoFootprint(heroInv, stashGo);
                    }
                } else {
                    // Inventory is open
                    if (!g_wasInventoryOpen) {
                        // Inventory was just reopened while Shared Stash was remembered as active
                        g_wasInventoryOpen = true;
                        if (heroInv && Readable(heroInv, 0xB0)) {
                            ZeroStashGoFootprint(heroInv, stashGo);
                            SetInventoryDbLocation(heroInv, stashGo, 0x15 /*il_main*/);
                            *(void**)((char*)heroInv + 0xA4) = stashGo;
                        }
                        if (!bpVis || activeBpGoid != g_localStashGoid) {
                            g_suppressBackpackSfx = true;
                            s_toggleBackpack(uiPartyMgr, 1, 0);
                            g_suppressBackpackSfx = false;
                            bpVis = eng::InterfaceVisible("DSX_backpack_ui");
                            activeBpGoid = *(uint32_t*)((char*)uiPartyMgr + 0x1E0);
                        }
                        SetStashUIText("Shared Stash");
                        g_lastTitleMode = 1;
                        g_lastTitleUpdate = now;
                    } else if (!bpVis) {
                        // Player clicked X on the Shared Stash while the inventory remained open
                        PlaySpellBookSfx();
                        CleanupClientStashUI();
                    } else if (heroInv && Readable(heroInv, 0xB0)) {
                        ZeroStashGoFootprint(heroInv, stashGo);
                        SetInventoryDbLocation(heroInv, stashGo, 0x15 /*il_main*/);
                    }
                }
            }
        }

        // Always keep the DSX_backpack_ui header text matched to whichever container is displayed
        if (bpVis) {
            bool isStashWindow = (g_sharedStashActive && g_localStashGoid != 0 && activeBpGoid == g_localStashGoid);
            int targetMode = isStashWindow ? 1 : 2;
            if (g_lastTitleMode != targetMode || (now - g_lastTitleUpdate >= 100)) {
                g_lastTitleMode = targetMode;
                g_lastTitleUpdate = now;
                SetStashUIText(isStashWindow ? "Shared Stash" : "Backpack");
            }
        }
    }

    // 3. Server tick: check for inventory modifications in active stashes every second
    static DWORD s_lastServerCheck = 0;
    if (now - s_lastServerCheck >= 1000) {
        s_lastServerCheck = now;
        for (auto& kv : g_serverStashes) {
            AccountStash& st = kv.second;
            void* stashGo = ResolveGo(st.stashGoid);
            if (!stashGo || !Readable(stashGo, 0x90)) continue;
            st.stashGo = stashGo;

            // Keep server-side stashInv->m_pGridbox null on dedicated server so client UIGridbox is authoritative
            if (Cfg().role == Role::Server) {
                void* stashInv = *(void**)((char*)stashGo + 0x2C);
                if (stashInv && Readable(stashInv, 0xA0)) {
                    *(void**)((char*)stashInv + 0x98) = nullptr;
                }
            }

            void** b = *(void***)((char*)stashGo + 0x88);
            void** e = *(void***)((char*)stashGo + 0x8C);
            int count = (b && e && e >= b) ? (int)(e - b) : 0;
            uint32_t h = ComputeStashHash(stashGo);

            if (count != st.lastItemCount || h != st.lastHash) {
                st.lastItemCount = count;
                st.lastHash      = h;
                SaveStashToFile(st.account, stashGo);
                st.lastSavedMs = now;
            }
        }
    }
}

// -------------------------------------------------------------------------------------------------
// Chat command filter: ONLY /stash and /sharedstash
// -------------------------------------------------------------------------------------------------
bool SharedStash_FilterKey(LPMSG msg) {
    if (!msg) return false;

    if (msg->message == WM_CHAR && g_eatNextReturnChar) {
        if (msg->wParam == VK_RETURN || msg->wParam == 0x0A || msg->wParam == 0x0D) {
            g_eatNextReturnChar = false;
            msg->message = WM_NULL;
            msg->wParam = 0;
            msg->lParam = 0;
            return true;
        }
    }

    if (msg->message == WM_KEYDOWN && msg->wParam == VK_RETURN) {
        if (eng::InterfaceVisible("game_console_mp") || eng::WindowVisible("console_edit", "game_console_mp")) {
            void* box = eng::FindWindow("console_edit", "game_console_mp");
            if (box && Readable(box, 0x1B0)) {
                int len = 0;
                const wchar_t* p = GetEditBoxWide(box, len);
                if (len > 0) {
                    std::wstring ws(p, len);
                    size_t first = ws.find_first_not_of(L" \t\r\n");
                    size_t last = ws.find_last_not_of(L" \t\r\n");
                    if (first != std::wstring::npos && last != std::wstring::npos) {
                        std::wstring cmd = ws.substr(first, last - first + 1);
                        if (_wcsicmp(cmd.c_str(), L"/stash") == 0 || _wcsicmp(cmd.c_str(), L"/sharedstash") == 0) {
                            // Eat Enter key
                            g_eatNextReturnChar = true;
                            msg->message = WM_NULL;
                            msg->wParam = 0;
                            msg->lParam = 0;

                            // Clear edit box
                            eng::GpStr emptyStr("");
                            eng::UIEditBox_SetText(box, &emptyStr.s);
                            eng::UIEditBox_SetPromptIndex(box, 0);
                            eng::UIEditBox_SetTextPosition(box, 0, 0);

                            // Cleanly close console via engine function
                            void* game = *(void**)0x007ACF8C;
                            if (game && Readable(game, 0x100)) {
                                s_closeConsole(game);
                            } else {
                                eng::HideInterface("game_console_mp");
                            }

                            // Clear UI input lock on WorldOptions
                            void* wo = *(void**)0x007ACFD4;
                            if (wo && Readable(wo, 0x50)) {
                                *((unsigned char*)wo + 0x46) = 0;
                            }

                            // Process stash command
                            SharedStash_TriggerOpen();
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

bool SharedStash_TriggerOpen() {
    void* hero = GetLocalScreenHero();
    if (!hero) {
        Log("SharedStash: could not find local hero");
        return false;
    }

    // Uber level requirement: must be > 30.0f
    float uberLevel = GetHeroUberLevel(hero);
    uint32_t heroGoid = *(uint32_t*)((char*)hero + 0x4C);
    Log("SharedStash: command triggered, hero 0x%08lX uberLevel=%.1f", heroGoid, uberLevel);

    if (uberLevel <= 30.0f) {
        g_pendingOpenStash = false;
        g_localStashGoid = 0;
        s_playVoice(hero, "order_out_of_mana", false);
        char buf[128];
        snprintf(buf, sizeof(buf), "Shared Stash requires Uber level above 30 (Current: %.1f).", uberLevel);
        PrintGameChat(buf);
        return true;
    }

    // Suppress spellbook SFX if inventory was not already opened before /stash
    bool wasInvOpen = IsInventoryOpen();
    if (!wasInvOpen) {
        g_suppressBackpackSfx = true;
    }

    // Immediately open the character inventory and replace spellbook/backpack if open
    OpenClientInventoryForStash(hero);

    // Try to open stash immediately if already known and resolved on client
    if (g_localStashGoid != 0) {
        void* stashGo = ResolveGo(g_localStashGoid);
        if (stashGo) {
            OpenClientStashUI(hero, stashGo);
            return true;
        } else {
            g_localStashGoid = 0;
        }
    }

    // If local host / singleplayer, create directly
    if (s_isServerLocal()) {
        void* stashGo = GetOrCreateServerStash("local", hero);
        if (stashGo) {
            g_localStashGoid = *(uint32_t*)((char*)stashGo + 0x4C);
            OpenClientStashUI(hero, stashGo);
            return true;
        }
    }

    // Request from server over DirectPlay
    g_pendingOpenStash = true;
    SendStashPacketToServer(heroGoid, kPkt_ReqOpen);
    return true;
}

bool SharedStash_FilterMouse(LPMSG msg) {
    if (!msg || msg->message != WM_LBUTTONDOWN) return false;
    if (s_isServerLocal() ? false : (Cfg().role == Role::Server)) return false;

    HWND hwnd = g_gameHwnd ? g_gameHwnd : msg->hwnd;
    POINT pt = msg->pt;
    if (hwnd) ScreenToClient(hwnd, &pt);
    int mx = pt.x, my = pt.y;

    void* ui = eng::UI();
    if (!ui) return false;

    void* stashBtn = eng::FindWindow("awp_stash_btn_1", "character_awp");
    if (stashBtn && *(bool*)((char*)stashBtn + 0x108)) {
        int left   = *(int*)((char*)stashBtn + 0x60) - 2;
        int top    = *(int*)((char*)stashBtn + 0x64) - 2;
        int right  = *(int*)((char*)stashBtn + 0x68) + 2;
        int bottom = *(int*)((char*)stashBtn + 0x6C) + 16;
        if (mx >= left && mx <= right && my >= top && my <= bottom) {
            SharedStash_TriggerOpen();
            msg->message = WM_NULL;
            msg->wParam = 0;
            msg->lParam = 0;
            return true;
        }
    }
    return false;
}
