// Raze RT: the state the scene code shares with the RTGL1 backend. Compiled
// with or without ray tracing support, so the scene hooks always link.

#include "rt_state.h"

FRtState rtstate;
bool     g_rtActive = false;
float    g_rtMuzzleFlash = 0.f;
int ( *RT_ActorRuleFlagsHook )( DCoreActor* actor ) = nullptr;
