#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <string>

void SharedStash_Init();
void SharedStash_SetDirectPlay(void* dpClient, void* dpServer);
void SharedStash_Tick();
bool SharedStash_FilterKey(LPMSG msg);
bool SharedStash_FilterMouse(LPMSG msg);
bool SharedStash_OnReceivePacket(bool isServer, DWORD senderDpnid, const BYTE* data, DWORD size);
void SharedStash_OnPlayerJoin(DWORD mach, const char* account);
void SharedStash_OnPlayerLeave(DWORD mach);
