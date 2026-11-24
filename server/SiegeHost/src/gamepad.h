#pragma once
#include <windows.h>

// Full XInput / DS4Windows-compatible gamepad support for Dungeon Siege MMO (client-only).
// Completely inert when Role=server, [Gamepad] Enabled=0, or no controller is connected.

void Gamepad_Init();
void Gamepad_Tick();
void Gamepad_FilterMessage(LPMSG msg);
bool Gamepad_IsVirtualCursorActive();
bool Gamepad_GetVirtualCursorClient(POINT* outPt);
