#pragma once

// Server-only real-time Uber Level-up announcer (posts to SiegePortal POST /api/shard/levelup).
// Completely dormant on Role::Client and Role::None.
void LevelAnnounce_Init();
void LevelAnnounce_Tick();
