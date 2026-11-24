// hosthide.h - hides the dedicated ServerHost dummy player from all player-facing UI,
// overhead nameplates, chat announcements, minimap, collision, AI perception, and 3D world view.
#pragma once

void HostHide_ClientInstall();
void HostHide_ClientTick();
void HostHide_ServerTick();
