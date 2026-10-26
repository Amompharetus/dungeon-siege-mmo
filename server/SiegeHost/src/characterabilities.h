#pragma once
#include <windows.h>
#include <cstdint>

void CharacterAbilities_Init();
void CharacterAbilities_Tick();
bool CharacterAbilities_FilterKey(LPMSG msg);
bool CharacterAbilities_OnReceivePacket(bool isServer, DWORD senderDpnid, const BYTE* data, DWORD size);
void CharacterAbilities_SetDirectPlay(void* dpClient, void* dpServer);
void CharacterAbilities_NotifyConnect(const char* hostUrl);
bool CharacterAbilities_IsServerSupported();
