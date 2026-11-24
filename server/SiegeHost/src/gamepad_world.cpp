#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "siegehost.h"
#include "engine.h"
#include "areagroups.h"
#include "characterabilities.h"
#include "sharedstash.h"
#include "gamepad_internal.h"

using namespace gp_internal;

namespace {

// ----------------------------------------------------------------------------------------------
// Engine function pointers (DSLOA.exe v1.50 verified from binary disassembly)
// ----------------------------------------------------------------------------------------------
typedef const SiegePos* (__thiscall* GoPlacementGetPosFn)(const void*);
typedef Vec3* (__thiscall* SiegePosWorldPosFn)(const SiegePos*, Vec3*);
typedef void  (__thiscall* SiegePosFromWorldPosFn)(SiegePos*, const Vec3*, const uint32_t*);
typedef void  (__thiscall* RapiWorldToScreenFn)(void*, Vec3*, const Vec3*);
typedef bool  (__thiscall* SiegeAdjustPointFn)(void*, SiegePos*, uint32_t, float);
typedef void  (__thiscall* GoMindRSMoveFn)(void*, const SiegePos*, int, int);
typedef void  (__thiscall* GoMindRSStopFn)(void*, int);
typedef bool  (__thiscall* GoMindIsEnemyFn)(void*, const void*);
typedef bool  (__thiscall* GoMindIsFriendFn)(void*, const void*);
typedef bool  (__thiscall* GoBoolQueryFn)(const void*);
typedef bool  (__thiscall* ContextActionOnGoFn)(void*, uint32_t, bool, int);
typedef uint8_t (__cdecl*  UIGamePotionFn)();
typedef uint32_t (__thiscall* CollectLootFn)(void*);
typedef uint32_t (__thiscall* RotateSlotsFn)(void*, bool);
typedef void* (__thiscall* ServerGetScreenHeroFn)(void*);

static const auto s_goPlacementGetPos    = (GoPlacementGetPosFn)0x004721CE;
static const auto s_siegePosWorldPos     = (SiegePosWorldPosFn)0x00671E3C;
static const auto s_siegePosFromWorldPos = (SiegePosFromWorldPosFn)0x00671E81;
static const auto s_rapiWorldToScreen    = (RapiWorldToScreenFn)0x0065F475;
static const auto s_siegeAdjustPoint     = (SiegeAdjustPointFn)0x0066FEAB;
static const auto s_goMindRSMove         = (GoMindRSMoveFn)0x005DE503;
static const auto s_goMindRSStop         = (GoMindRSStopFn)0x005DE632;
static const auto s_goMindIsEnemy        = (GoMindIsEnemyFn)0x005DF1BE;
static const auto s_goMindIsFriend       = (GoMindIsFriendFn)0x005DF22D;
static const auto s_goIsInFrustum        = (GoBoolQueryFn)0x00539161;
static const auto s_goIsActor            = (GoBoolQueryFn)0x00539442;
static const auto s_goIsBreakable        = (GoBoolQueryFn)0x005394AA;
static const auto s_goIsContainer        = (GoBoolQueryFn)0x0053952E;
static const auto s_goIsItem             = (GoBoolQueryFn)0x00539612;
static const auto s_goIsSelectable       = (GoBoolQueryFn)0x00539743;
static const auto s_goIsUsable           = (GoBoolQueryFn)0x005397C4;
static const auto s_goIsInsideInv        = (GoBoolQueryFn)0x0047C07D;
static const auto s_contextActionOnGo    = (ContextActionOnGoFn)0x00512EF0;
static const auto s_uiGameHealBody       = (UIGamePotionFn)0x004EEA77;
static const auto s_uiGameHealMana       = (UIGamePotionFn)0x004EEB16;
static const auto s_partyCollectLoot     = (CollectLootFn)0x004A8D71;
static const auto s_rotateSelectedSlots  = (RotateSlotsFn)0x0050387C;
static const auto s_serverGetScreenHero  = (ServerGetScreenHeroFn)0x0056DB34;

// ----------------------------------------------------------------------------------------------
// UICamera::Update (0x0047C93E) 9-byte trampoline hook for smooth analog camera orbit/pitch/zoom
// ----------------------------------------------------------------------------------------------
typedef void (__thiscall* UICameraUpdateFn)(void* cam, float dt);
UICameraUpdateFn g_origUICameraUpdate = nullptr;
bool g_uiCameraHooked = false;

float g_analogCamYaw   = 0.0f; // [-1, 1], + = rotate right
float g_analogCamPitch = 0.0f; // [-1, 1], + = tilt up
float g_analogCamZoom  = 0.0f; // [-1, 1], + = zoom in

static void __attribute__((thiscall)) HookUICameraUpdate(void* cam, float dt) {
    if (!g_origUICameraUpdate) return;

    bool apply = (cam && Readable(cam, 0xA0)) &&
                 (std::fabs(g_analogCamYaw) > 0.001f ||
                  std::fabs(g_analogCamPitch) > 0.001f ||
                  std::fabs(g_analogCamZoom) > 0.001f);

    if (!apply) {
        g_origUICameraUpdate(cam, dt);
        return;
    }

    char* c = (char*)cam;
    // Save vanilla keyboard camera flags & speed multipliers so keyboard/mouse wheel never get stuck
    char savedFlags[6];
    memcpy(savedFlags, c + 0x80, 6);
    float savedRotMult   = *(float*)(c + 0x8C);
    float savedPitchMult = *(float*)(c + 0x90);
    float savedZoomRate  = *(float*)(c + 0x5C);

    float speed = Cfg().gamepadCameraSpeed;
    if (speed < 0.2f) speed = 0.2f;
    if (speed > 4.0f) speed = 4.0f;

    float yaw = Cfg().gamepadInvertCamX ? -g_analogCamYaw : g_analogCamYaw;
    float pitch = Cfg().gamepadInvertCamY ? -g_analogCamPitch : g_analogCamPitch;
    float zoom = g_analogCamZoom;

    if (std::fabs(yaw) > 0.001f) {
        c[0x80] = (yaw > 0.0f) ? 1 : 0; // rotate right
        c[0x81] = (yaw < 0.0f) ? 1 : 0; // rotate left
        *(float*)(c + 0x8C) = (savedRotMult > 0.05f ? savedRotMult : 1.0f) * std::fabs(yaw) * speed * 1.35f;
    }
    if (std::fabs(pitch) > 0.001f) {
        c[0x82] = (pitch > 0.0f) ? 1 : 0; // tilt up
        c[0x83] = (pitch < 0.0f) ? 1 : 0; // tilt down
        *(float*)(c + 0x90) = (savedPitchMult > 0.05f ? savedPitchMult : 1.0f) * std::fabs(pitch) * speed * 1.15f;
    }
    if (std::fabs(zoom) > 0.001f) {
        c[0x85] = (zoom > 0.0f) ? 1 : 0; // zoom in
        c[0x84] = (zoom < 0.0f) ? 1 : 0; // zoom out
        *(float*)(c + 0x5C) = (savedZoomRate > 0.05f ? savedZoomRate : 12.0f) * std::fabs(zoom) * speed;
    }

    g_origUICameraUpdate(cam, dt);

    // Restore original flags & multipliers immediately
    memcpy(c + 0x80, savedFlags, 6);
    *(float*)(c + 0x8C) = savedRotMult;
    *(float*)(c + 0x90) = savedPitchMult;
    *(float*)(c + 0x5C) = savedZoomRate;
}

static void InstallUICameraHook() {
    if (g_uiCameraHooked) return;
    const DWORD kTarget = 0x0047C93E;
    // Verified prologue: push ebp; mov ebp, esp; sub esp, 0xA0
    static const BYTE kExpect[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00};
    if (memcmp((const void*)kTarget, kExpect, sizeof(kExpect)) != 0) {
        Log("Gamepad: UICamera::Update prologue mismatch at 0x%08lX; analog camera hook skipped.", kTarget);
        return;
    }

    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return;
    memcpy(tramp, kExpect, sizeof(kExpect));
    tramp[9] = 0xE9;
    *(DWORD*)(tramp + 10) = (DWORD)((kTarget + sizeof(kExpect)) - ((DWORD)(tramp + 9) + 5));
    g_origUICameraUpdate = (UICameraUpdateFn)tramp;

    BYTE patch[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    *(DWORD*)(patch + 1) = (DWORD)((BYTE*)&HookUICameraUpdate - (BYTE*)(kTarget + 5));

    DWORD old = 0;
    if (!VirtualProtect((LPVOID)kTarget, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) {
        g_origUICameraUpdate = nullptr;
        return;
    }
    memcpy((void*)kTarget, patch, sizeof(patch));
    VirtualProtect((LPVOID)kTarget, sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kTarget, sizeof(patch));
    g_uiCameraHooked = true;
    Log("Gamepad: hooked UICamera::Update at 0x%08lX (analog camera orbit/pitch/zoom active).", kTarget);
}

// ----------------------------------------------------------------------------------------------
// Movement & Targeting runtime state
// ----------------------------------------------------------------------------------------------
bool     g_wasMovingByStick = false;
DWORD    g_lastMoveOrderMs  = 0;
float    g_lastMoveDirX     = 0.0f;
float    g_lastMoveDirZ     = 0.0f;
float    g_lastFacingX      = 0.0f;
float    g_lastFacingZ      = 1.0f;
bool     g_hasLastFacing    = false;

uint32_t g_lockedTargetGoid = 0;
bool     g_manualTargetLock = false;
uint32_t g_highlightedGoid  = 0;
bool     g_freeAimInitialized = false;

static void* GetRapiRenderer() {
    uintptr_t appMod = *(uintptr_t*)kAddrAppModule;
    if (!appMod || !Readable((const void*)(appMod + 0x1E0), 4)) return nullptr;
    void* rapi = *(void**)(appMod + 0x1E0);
    return (rapi && Readable(rapi, 0xD00)) ? rapi : nullptr;
}

static bool ProjectWorldToScreen(const Vec3& worldPos, float& outX, float& outY) {
    void* rapi = GetRapiRenderer();
    if (!rapi) return false;

    // Check point is in front of camera before projecting so behind-camera points never mirror onto screen
    void* siegeEngine = *(void**)kAddrSiegeEngine;
    if (siegeEngine && Readable(siegeEngine, 0x44)) {
        void* cam = *(void**)((char*)siegeEngine + 0x3C);
        if (cam && Readable(cam, 0x168)) {
            const Vec3& camPos = *(const Vec3*)((const char*)cam + 0x148);
            const Vec3& tgtPos = *(const Vec3*)((const char*)cam + 0x154);
            float fx = tgtPos.x - camPos.x;
            float fy = tgtPos.y - camPos.y;
            float fz = tgtPos.z - camPos.z;
            float dx = worldPos.x - camPos.x;
            float dy = worldPos.y - camPos.y;
            float dz = worldPos.z - camPos.z;
            if (fx * dx + fy * dy + fz * dz <= 0.2f) return false;
        }
    }

    Vec3 screenOut = {0.0f, 0.0f, 0.0f};
    s_rapiWorldToScreen(rapi, &screenOut, &worldPos);
    if (!std::isfinite(screenOut.x) || !std::isfinite(screenOut.y)) return false;

    int sw = 0, sh = 0;
    GetScreenDimensions(sw, sh);
    outX = screenOut.x;
    outY = screenOut.y;
    return (outX >= 0.0f && outX <= (float)sw && outY >= 0.0f && outY <= (float)sh);
}

static bool ComputeScreenBasisXZ(const Vec3& heroWorld, float& fwdX, float& fwdZ, float& rightX, float& rightZ) {
    void* siegeEngine = *(void**)kAddrSiegeEngine;
    if (!siegeEngine || !Readable(siegeEngine, 0x44)) return false;
    void* cam = *(void**)((char*)siegeEngine + 0x3C);
    if (!cam || !Readable(cam, 0x168)) return false;

    const Vec3& camPos = *(const Vec3*)((const char*)cam + 0x148);
    const Vec3& tgtPos = *(const Vec3*)((const char*)cam + 0x154);

    fwdX = tgtPos.x - camPos.x;
    fwdZ = tgtPos.z - camPos.z;
    float fl = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
    if (fl < 1e-4f) {
        fwdX = 0.0f;
        fwdZ = 1.0f;
    } else {
        fwdX /= fl;
        fwdZ /= fl;
    }

    rightX = fwdZ;
    rightZ = -fwdX;

    // Self-verify orientation using Rapi::WorldToScreen so stick-right ALWAYS moves screen-right
    // and stick-up ALWAYS moves screen-up regardless of D3D7/Siege handedness!
    float hx = 0.0f, hy = 0.0f;
    float rx = 0.0f, ry = 0.0f;
    float fx = 0.0f, fy = 0.0f;
    if (ProjectWorldToScreen(heroWorld, hx, hy)) {
        Vec3 testR = {heroWorld.x + rightX * 1.5f, heroWorld.y, heroWorld.z + rightZ * 1.5f};
        Vec3 testF = {heroWorld.x + fwdX   * 1.5f, heroWorld.y, heroWorld.z + fwdZ   * 1.5f};
        if (ProjectWorldToScreen(testR, rx, ry) && rx < hx) {
            rightX = -rightX;
            rightZ = -rightZ;
        }
        if (ProjectWorldToScreen(testF, fx, fy) && fy > hy) {
            fwdX = -fwdX;
            fwdZ = -fwdZ;
        }
    }
    return true;
}

static bool IsGoConscious(void* go) {
    if (!go || !Readable(go, 0x80)) return false;
    void* aspect = *(void**)((char*)go + 0x04);
    if (!aspect || !Readable(aspect, 0xB8)) return false;
    return (*(const int*)((const char*)aspect + 0xA0) == 1) && (*(const float*)((const char*)aspect + 0xB0) > 0.0f);
}

static void ReadGoScreenName(void* go, char* outBuf, size_t cap) {
    if (!outBuf || cap == 0) return;
    outBuf[0] = '\0';
    if (!go || !Readable(go, 0x40)) return;
    void* common = *(void**)((char*)go + 0x10);
    if (!common || !Readable(common, 0x20)) return;
    const wchar_t* const* ppName = *(const wchar_t* const**)((const char*)common + 0x14);
    const wchar_t* wName = (ppName && Readable(ppName, sizeof(void*))) ? *ppName : nullptr;
    if (wName && Readable(wName, sizeof(wchar_t)) && *wName) {
        WideCharToMultiByte(CP_ACP, 0, wName, -1, outBuf, (int)cap - 1, nullptr, nullptr);
        outBuf[cap - 1] = '\0';
    }
}

static void UpdateAnalogMovement(void* hero, const PadFrame& pad) {
    if (!hero || !IsGoConscious(hero)) {
        g_wasMovingByStick = false;
        return;
    }

    void* heroMind = *(void**)((char*)hero + 0x34);
    void* placement = *(void**)((char*)hero + 0x40);
    void* actor = *(void**)((char*)hero + 0x00);
    void* siegeEngine = *(void**)kAddrSiegeEngine;
    if (!heroMind || !placement || !siegeEngine ||
        !Readable(heroMind, 0x60) || !Readable(placement, 0x30) || !Readable(siegeEngine, 0x44)) {
        return;
    }

    // Never fight potion-drinking animation
    if (IsActorDrinkingPotion(hero)) return;

    if (pad.leftStick.magnitude <= 0.0f) {
        if (g_wasMovingByStick) {
            g_wasMovingByStick = false;
            s_goMindRSStop(heroMind, 3 /*AO_USER*/);
        }
        return;
    }

    const SiegePos* heroPos = s_goPlacementGetPos(placement);
    if (!heroPos || !Readable(heroPos, sizeof(SiegePos))) return;

    Vec3 heroWorld = {0.0f, 0.0f, 0.0f};
    s_siegePosWorldPos(heroPos, &heroWorld);

    float fwdX = 0.0f, fwdZ = 1.0f, rightX = 1.0f, rightZ = 0.0f;
    if (!ComputeScreenBasisXZ(heroWorld, fwdX, fwdZ, rightX, rightZ)) return;

    float moveDirX = 0.0f, moveDirZ = 0.0f;
    if (!gamepad::ComputeCameraRelativeMoveDir(fwdX, fwdZ, rightX, rightZ,
                                               pad.leftStick.x, pad.leftStick.y,
                                               moveDirX, moveDirZ)) {
        return;
    }

    g_lastFacingX = moveDirX;
    g_lastFacingZ = moveDirZ;
    g_hasLastFacing = true;

    // Throttle RSMove RPCs to ~115ms (or immediately on sharp turn > ~28 deg) to keep NetFuBiSend light
    float dirDot = g_wasMovingByStick ? (moveDirX * g_lastMoveDirX + moveDirZ * g_lastMoveDirZ) : -1.0f;
    bool sharpTurn = (dirDot < 0.88f);
    if (g_wasMovingByStick && !sharpTurn && (DWORD)(pad.nowMs - g_lastMoveOrderMs) < 115) {
        return;
    }

    // Hero's own logical walkability flags from GoActor+0x30 (fallback 1 = LF_HUMAN_PLAYER)
    uint32_t walkFlags = 1;
    if (actor && Readable(actor, 0x38)) {
        uint32_t f = *(const uint32_t*)((const char*)actor + 0x30);
        if (f != 0) walkFlags = f;
    }

    float baseDist = 1.5f + 1.4f * pad.leftStick.magnitude;
    auto probes = gamepad::GenerateWallSlideOffsets(moveDirX, moveDirZ, baseDist);

    SiegePos chosenPos{};
    bool found = false;
    for (const auto& pr : probes) {
        Vec3 candWorld = {heroWorld.x + pr.dx, heroWorld.y, heroWorld.z + pr.dz};
        SiegePos candPos{};
        uint32_t seedNode = heroPos->node;
        s_siegePosFromWorldPos(&candPos, &candWorld, &seedNode);

        if (s_siegeAdjustPoint(siegeEngine, &candPos, walkFlags, 1.8f)) {
            chosenPos = candPos;
            found = true;
            break;
        }
    }

    if (found) {
        s_goMindRSMove(heroMind, &chosenPos, 0 /*QP_CLEAR*/, 3 /*AO_USER*/);
        g_wasMovingByStick = true;
        g_lastMoveOrderMs = pad.nowMs;
        g_lastMoveDirX = moveDirX;
        g_lastMoveDirZ = moveDirZ;
    }
}

static void ScanAndHighlightTargets(void* hero, const PadFrame& pad,
                                    std::vector<gamepad::TargetCandidate>& outCandidates,
                                    int& outBestIdx,
                                    std::string& outBestName)
{
    outCandidates.clear();
    outBestIdx = -1;
    outBestName.clear();
    if (!hero || !Readable(hero, 0x80)) return;

    void* godb = *(void**)kAddrGoDb;
    void* heroMind = *(void**)((char*)hero + 0x34);
    void* heroPlacement = *(void**)((char*)hero + 0x40);
    if (!godb || !heroPlacement || !Readable(godb, 0x340) || !Readable(heroPlacement, 0x30)) return;

    const SiegePos* heroPos = s_goPlacementGetPos(heroPlacement);
    if (!heroPos) return;
    Vec3 heroWorld{};
    s_siegePosWorldPos(heroPos, &heroWorld);

    uint32_t heroMask = *(const uint32_t*)((const char*)hero + 0x68);
    void* head = *(void**)((char*)godb + 0x338);
    if (!head || !Readable(head, 0x80)) return;
    void* first = *(void**)((char*)head + 0x70);
    if (!first || !Readable(first, 0x80)) return;

    std::vector<void*> candidateGos;
    outCandidates.reserve(48);
    candidateGos.reserve(48);

    int guard = 0;
    for (void* go = first; go && go != head && guard < 4096 && outCandidates.size() < 48; ++guard) {
        if (!Readable(go, 0xA0)) break;
        void* next = *(void**)((char*)go + 0x70);

        if (go != hero && ((*((const uint8_t*)go + 0x96) & 8) == 0)) {
            uint32_t mask = *(const uint32_t*)((const char*)go + 0x68);
            if ((heroMask & mask) != 0 && s_goIsSelectable(go) && !s_goIsInsideInv(go)) {
                void* pl = *(void**)((char*)go + 0x40);
                uint32_t goid = *(const uint32_t*)((const char*)go + 0x4C);
                if (pl && goid != 0 && Readable(pl, 0x30)) {
                    const SiegePos* pos = s_goPlacementGetPos(pl);
                    if (pos) {
                        Vec3 wPos{};
                        s_siegePosWorldPos(pos, &wPos);
                        float dx = wPos.x - heroWorld.x;
                        float dy = wPos.y - heroWorld.y;
                        float dz = wPos.z - heroWorld.z;
                        float distSq = dx * dx + dy * dy + dz * dz;

                        if (distSq <= 18.0f * 18.0f && distSq > 0.01f) {
                            gamepad::TargetKind kind = gamepad::TargetKind::None;
                            bool isActor = s_goIsActor(go);
                            void* aspect = *(void**)((char*)go + 0x04);

                            if (isActor) {
                                if (aspect && Readable(aspect, 0xB8) &&
                                    *(const int*)((const char*)aspect + 0xA0) == 1 &&
                                    (*(const uint32_t*)((const char*)aspect + 0x3C) & 0x100) == 0)
                                {
                                    bool isEnemy = (heroMind && Readable(heroMind, 0x60)) ? s_goMindIsEnemy(heroMind, go) : false;
                                    if (isEnemy) {
                                        kind = gamepad::TargetKind::Enemy;
                                    } else {
                                        void* player = *(void**)((char*)go + 0x78);
                                        int playerType = (player && Readable(player, 0x20)) ? *(const int*)((const char*)player + 0x14) : 0;
                                        if (playerType == 2) {
                                            // Friendly NPC (shopkeeper / quest giver / companion)
                                            if (*(void**)((char*)go + 0x14) != nullptr || *(void**)((char*)go + 0x48) != nullptr) {
                                                kind = gamepad::TargetKind::Interactable;
                                            }
                                        } else if (playerType == 1 && distSq <= 6.5f * 6.5f) {
                                            // Another human player (trade)
                                            const wchar_t* pn = *(const wchar_t**)((const char*)player + 0x58);
                                            bool isHost = (pn && Readable(pn, 4) && _wcsicmp(pn, L"ServerHost") == 0);
                                            if (!isHost) kind = gamepad::TargetKind::AllyPlayer;
                                        }
                                    }
                                }
                            } else if (s_goIsUsable(go)) {
                                if (distSq <= 9.0f * 9.0f) kind = gamepad::TargetKind::Interactable;
                            } else if (s_goIsContainer(go) && !s_goIsBreakable(go)) {
                                if (distSq <= 9.0f * 9.0f) kind = gamepad::TargetKind::Interactable;
                            } else if (s_goIsBreakable(go)) {
                                if (distSq <= 8.0f * 8.0f && aspect && Readable(aspect, 0xB0) &&
                                    *(const int*)((const char*)aspect + 0xA0) == 1)
                                {
                                    kind = gamepad::TargetKind::Breakable;
                                }
                            } else if (s_goIsItem(go)) {
                                if (distSq <= 7.0f * 7.0f) kind = gamepad::TargetKind::GroundItem;
                            }

                            if (kind != gamepad::TargetKind::None) {
                                float dist = std::sqrt(distSq);
                                float xzLen = std::sqrt(dx * dx + dz * dz);
                                float ndx = (xzLen > 1e-4f) ? (dx / xzLen) : 0.0f;
                                float ndz = (xzLen > 1e-4f) ? (dz / xzLen) : 0.0f;

                                Vec3 projPos = {wPos.x, wPos.y + (isActor ? 1.15f : 0.45f), wPos.z};
                                float sx = 0.0f, sy = 0.0f;
                                bool onScr = ProjectWorldToScreen(projPos, sx, sy);

                                gamepad::TargetCandidate tc{};
                                tc.goid = goid;
                                tc.kind = kind;
                                tc.dist = dist;
                                tc.dirX = ndx;
                                tc.dirZ = ndz;
                                tc.screenX = sx;
                                tc.screenY = sy;
                                tc.onScreen = onScr;
                                outCandidates.push_back(tc);
                                candidateGos.push_back(go);
                            }
                        }
                    }
                }
            }
        }
        go = next;
    }

    // Validate existing manual target lock
    if (g_manualTargetLock && g_lockedTargetGoid != 0) {
        bool stillPresent = false;
        for (const auto& c : outCandidates) {
            if (c.goid == g_lockedTargetGoid) { stillPresent = true; break; }
        }
        if (!stillPresent) {
            g_manualTargetLock = false;
            g_lockedTargetGoid = 0;
        }
    }

    // R3 cycles / locks targets
    if (pad.Pressed(xinput_abi::kRightThumb)) {
        uint32_t nextGoid = gamepad::CycleNextTargetGoid(outCandidates, g_highlightedGoid, +1);
        if (nextGoid != 0) {
            g_lockedTargetGoid = nextGoid;
            g_manualTargetLock = true;
        } else {
            g_lockedTargetGoid = 0;
            g_manualTargetLock = false;
        }
    }

    outBestIdx = gamepad::SelectBestTargetIndex(
        outCandidates, g_lastFacingX, g_lastFacingZ, g_hasLastFacing,
        g_manualTargetLock ? g_lockedTargetGoid : g_highlightedGoid,
        g_manualTargetLock);

    if (outBestIdx >= 0) {
        const auto& best = outCandidates[outBestIdx];
        g_highlightedGoid = best.goid;

        char nm[64] = "";
        ReadGoScreenName(candidateGos[outBestIdx], nm, sizeof(nm));
        outBestName = nm;

        if (best.onScreen) {
            int half = (best.kind == gamepad::TargetKind::Enemy) ? 20 : 16;
            g_overlay.showTargetBox = true;
            g_overlay.targetRect = {
                (int)std::lround(best.screenX) - half,
                (int)std::lround(best.screenY) - half,
                (int)std::lround(best.screenX) + half,
                (int)std::lround(best.screenY) + half
            };
            switch (best.kind) {
                case gamepad::TargetKind::Enemy:
                    g_overlay.targetColor = g_manualTargetLock ? 0xFFFF2222 : 0xFFFF6644;
                    break;
                case gamepad::TargetKind::Interactable:
                    g_overlay.targetColor = 0xFF44E5FF;
                    break;
                case gamepad::TargetKind::Breakable:
                    g_overlay.targetColor = 0xFFFFAA33;
                    break;
                case gamepad::TargetKind::GroundItem:
                    g_overlay.targetColor = 0xFF66FF88;
                    break;
                case gamepad::TargetKind::AllyPlayer:
                    g_overlay.targetColor = 0xFF88BBFF;
                    break;
                default:
                    g_overlay.targetColor = 0xFFFFFFFF;
                    break;
            }
        }
    } else {
        g_highlightedGoid = 0;
    }
}

static void ExecuteContextAction(uint32_t goid) {
    if (!goid) return;
    void* uiGame = *(void**)kAddrUIGame;
    if (!uiGame || !Readable(uiGame, 0x30)) return;
    void* uiCmds = *(void**)((char*)uiGame + 0x1C);
    if (!uiCmds || !Readable(uiCmds, 0x40)) return;
    g_wasMovingByStick = false; // Do not override attack/interact pathing with RSStop
    s_contextActionOnGo(uiCmds, goid, false, 3 /*AO_USER*/);
}

static void RotateWeaponSlot(bool prev) {
    void* uiGame = *(void**)kAddrUIGame;
    if (!uiGame || !Readable(uiGame, 0x30)) return;
    void* uiInvMgr = *(void**)((char*)uiGame + 0x24);
    if (!uiInvMgr || !Readable(uiInvMgr, 0x40)) return;
    s_rotateSelectedSlots(uiInvMgr, prev);
}

} // namespace

// ----------------------------------------------------------------------------------------------
// gp_internal World Interface
// ----------------------------------------------------------------------------------------------
namespace gp_internal {

void GamepadWorld_InstallHooks() {
    InstallUICameraHook();
}

void* GamepadWorld_GetLocalHero() {
    void* srv = *(void**)kAddrServer;
    if (!srv || !Readable(srv, 0x20)) return nullptr;
    void* hero = s_serverGetScreenHero(srv);
    return (hero && Readable(hero, 0x80)) ? hero : nullptr;
}

bool GamepadWorld_IsHeroInTown(void* hero) {
    if (void* wm = eng::WorldMap_Singleton()) {
        if (Readable(wm, 0x10)) {
            const char* mapName = eng::GpC(wm, 4);
            if (mapName && *mapName && _stricmp(mapName, "multiplayer_world") != 0) {
                return false;
            }
        }
    }
    void* srv = *(void**)kAddrServer;
    if (!srv || !Readable(srv, 0x20)) return false;
    void* pl = eng::Server_GetLocalHumanPlayer(srv);
    if (!pl || !Readable(pl, 0x100) || !eng::Player_IsValid(pl)) return false;
    unsigned loc = *(const unsigned*)((const char*)pl + 0xF4);
    return areagroups::IsTownLocation(loc);
}

bool GamepadWorld_GetCameraAnalog(float& outYaw, float& outPitch, float& outZoom) {
    outYaw   = g_analogCamYaw;
    outPitch = g_analogCamPitch;
    outZoom  = g_analogCamZoom;
    return (std::fabs(outYaw) > 0.001f || std::fabs(outPitch) > 0.001f || std::fabs(outZoom) > 0.001f);
}

void GamepadWorld_OnLeaveGameplay() {
    g_analogCamYaw   = 0.0f;
    g_analogCamPitch = 0.0f;
    g_analogCamZoom  = 0.0f;
    g_freeAimInitialized = false;
    if (g_wasMovingByStick) {
        g_wasMovingByStick = false;
        if (void* hero = GamepadWorld_GetLocalHero()) {
            void* mind = *(void**)((char*)hero + 0x34);
            if (mind && Readable(mind, 0x60)) {
                s_goMindRSStop(mind, 3 /*AO_USER*/);
            }
        }
    }
}

void GamepadWorld_TickGameplay(const PadFrame& pad) {
    g_freeAimInitialized = false;
    void* hero = GamepadWorld_GetLocalHero();

    // 1. Right Stick analog camera (LT + Right Stick Y = smooth analog zoom!)
    if (pad.ltDown) {
        g_analogCamYaw   = pad.rightStick.x;
        g_analogCamPitch = 0.0f;
        g_analogCamZoom  = pad.rightStick.y;
    } else {
        g_analogCamYaw   = pad.rightStick.x;
        g_analogCamPitch = pad.rightStick.y;
        g_analogCamZoom  = 0.0f;
    }

    // 2. Left Stick camera-relative analog movement
    UpdateAnalogMovement(hero, pad);

    // 3. Smart target/interact scan & highlight reticle
    std::vector<gamepad::TargetCandidate> candidates;
    int bestIdx = -1;
    std::string bestName;
    ScanAndHighlightTargets(hero, pad, candidates, bestIdx, bestName);

    // 4. Button actions
    if (pad.ltDown) {
        // LT chord: Q / W / E / R Custom MMO Abilities + Spellbook + Journal
        if (pad.Pressed(xinput_abi::kA)) CharacterAbilities_QueueTrigger(0); // [Q] Whirling Cleave
        if (pad.Pressed(xinput_abi::kB)) CharacterAbilities_QueueTrigger(1); // [W] Shrapnel Blast
        if (pad.Pressed(xinput_abi::kX)) CharacterAbilities_QueueTrigger(2); // [E] Aegis of Renewal
        if (pad.Pressed(xinput_abi::kY)) CharacterAbilities_QueueTrigger(3); // [R] Flame Nova
        if (pad.Pressed(xinput_abi::kDpadRight)) InjectKeyTap('B');          // Spellbook
        if (pad.Pressed(xinput_abi::kDpadLeft))  InjectKeyTap('J');          // Journal
        if (pad.Pressed(xinput_abi::kDpadUp))    InjectKeyTap(VK_OEM_1, true); // Shift+; BuffUI

        g_overlay.showHint = true;
        g_overlay.hintText = "[LT+A] Q-Cleave  [LT+B] W-Blast  [LT+X] E-Aegis  [LT+Y] R-Nova  [RS-Y] Zoom";
        return;
    }

    // Primary Context Action (Attack / Cast / Talk / Shop / Open Chest/Door / Shrine / Pick Up / Trade)
    if (pad.Pressed(xinput_abi::kA) && bestIdx >= 0) {
        ExecuteContextAction(candidates[bestIdx].goid);
    }

    // Health Potion (B) & Mana Potion (Y) — respects PvP potion block at 0x005F4C5D automatically
    if (pad.Pressed(xinput_abi::kB)) {
        s_uiGameHealBody();
    }
    if (pad.Pressed(xinput_abi::kY)) {
        s_uiGameHealMana();
    }

    // Collect Nearby Loot (X)
    if (pad.Pressed(xinput_abi::kX)) {
        void* partyMgr = *(void**)kAddrUIPartyMgr;
        if (partyMgr && Readable(partyMgr, 0x40)) {
            s_partyCollectLoot(partyMgr);
        }
    }

    // Cycle Active Weapon / Spell Slots (LB / RB)
    if (pad.Pressed(xinput_abi::kRightShoulder)) {
        RotateWeaponSlot(false);
    }
    if (pad.Pressed(xinput_abi::kLeftShoulder)) {
        RotateWeaponSlot(true);
    }

    // D-Pad shortcuts
    if (pad.Pressed(xinput_abi::kDpadLeft)) {
        InjectKeyTap('I'); // Inventory
    }
    if (pad.Pressed(xinput_abi::kDpadRight)) {
        if (GamepadWorld_IsHeroInTown(hero)) {
            SharedStash_TriggerOpen();
        } else {
            InjectKeyTap('B'); // Spellbook
        }
    }
    if (pad.Pressed(xinput_abi::kDpadUp)) {
        InjectKeyTap(VK_TAB); // MegaMap
    }
    if (pad.Pressed(xinput_abi::kDpadDown)) {
        InjectKeyTap(VK_MENU); // Item Labels (Alt)
    }

    // L3 toggles camera track/free mode ('T')
    if (pad.Pressed(xinput_abi::kLeftThumb)) {
        InjectKeyTap('T');
    }

    // Start opens Pause / In-Game Menu (Esc)
    if (pad.Pressed(xinput_abi::kStart)) {
        InjectKeyTap(VK_ESCAPE);
    }

    // Context-aware bottom hint strip
    g_overlay.showHint = true;
    if (bestIdx >= 0) {
        const char* verb = "Interact";
        switch (candidates[bestIdx].kind) {
            case gamepad::TargetKind::Enemy:        verb = "Attack"; break;
            case gamepad::TargetKind::Interactable: verb = "Use / Talk"; break;
            case gamepad::TargetKind::Breakable:    verb = "Smash"; break;
            case gamepad::TargetKind::GroundItem:   verb = "Pick Up"; break;
            case gamepad::TargetKind::AllyPlayer:   verb = "Trade"; break;
            default: break;
        }
        char buf[160];
        if (!bestName.empty()) {
            snprintf(buf, sizeof(buf), "[A] %s: %s  [X] Loot  [B/Y] Potions  [LT] Abilities  [RT] Aim",
                     verb, bestName.c_str());
        } else {
            snprintf(buf, sizeof(buf), "[A] %s  [X] Loot  [B/Y] Potions  [LT] Abilities  [RT] Aim", verb);
        }
        g_overlay.hintText = buf;
    } else {
        g_overlay.hintText = "[LS] Move  [RS] Camera  [X] Loot  [B/Y] Potions  [LB/RB] Weapons  [LT] Abilities";
    }
}

void GamepadWorld_TickFreeAim(const PadFrame& pad) {
    // Free-Aim Mode (hold RT): lets mages/ranged/all players precisely aim ground AoE spells,
    // click distant ground items (showing Ground Tooltip!), or interact with exact world points.
    g_analogCamYaw   = pad.rightStick.x;
    g_analogCamPitch = pad.rightStick.y;
    g_analogCamZoom  = 0.0f;

    int sw = 0, sh = 0;
    GetScreenDimensions(sw, sh);

    if (!g_freeAimInitialized) {
        g_freeAimInitialized = true;
        // Stop stick walking while aiming ground reticle
        if (g_wasMovingByStick) {
            g_wasMovingByStick = false;
            if (void* hero = GamepadWorld_GetLocalHero()) {
                void* mind = *(void**)((char*)hero + 0x34);
                if (mind && Readable(mind, 0x60)) s_goMindRSStop(mind, 3);
            }
        }
        // Start reticle near center of screen (slightly above hero)
        SetVirtualCursorClient(sw * 0.5f, sh * 0.45f, true, true);
    }

    float cx = 0.0f, cy = 0.0f;
    GetVirtualCursorClientF(cx, cy);
    gamepad::IntegrateVirtualCursor(cx, cy, pad.leftStick, pad.dtSec, sw, sh,
                                    Cfg().gamepadCursorSpeed, false, false);
    SetVirtualCursorClient(cx, cy, true, true);

    // Draw crisp gold aiming reticle around cursor
    int ix = (int)std::lround(cx);
    int iy = (int)std::lround(cy);
    g_overlay.showTargetBox = true;
    g_overlay.targetRect = {ix - 12, iy - 12, ix + 12, iy + 12};
    g_overlay.targetColor = 0xFFFFD700;

    // A = Left-click (move/attack/cast/use at cursor), X = Right-click (context/cast)
    if (pad.Pressed(xinput_abi::kA)) {
        InjectMouseTap(false, false);
    }
    if (pad.Pressed(xinput_abi::kX)) {
        InjectMouseTap(true, false);
    }
    if (pad.Pressed(xinput_abi::kB)) {
        s_uiGameHealBody();
    }
    if (pad.Pressed(xinput_abi::kY)) {
        s_uiGameHealMana();
    }
    if (pad.Pressed(xinput_abi::kRightShoulder)) {
        RotateWeaponSlot(false);
    }
    if (pad.Pressed(xinput_abi::kLeftShoulder)) {
        RotateWeaponSlot(true);
    }

    g_overlay.showHint = true;
    g_overlay.hintText = "[RT Aim]  [LS] Aim Cursor  [A] Cast / Click  [X] Context  [RS] Camera";
}

} // namespace gp_internal
