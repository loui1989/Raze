// Raze RT: the state the scene code shares with the RTGL1 backend. Compiled
// with or without ray tracing support, so the scene hooks always link.

#include "rt_state.h"

FRtState rtstate;
bool     g_rtActive = false;
float    g_rtMuzzleFlash = 0.f;
bool     g_rtCameraUnderwater = false;
uint32_t    g_rtShotCounter = 0;
const char* g_rtShotEvent   = nullptr;
const void* g_rtViewerActor = nullptr;
int         g_rtViewSector  = -1;
std::vector< uint8_t > g_rtCopySector;
std::vector< RtGameLight > g_rtGameLights;
std::vector< RtCopyLink >  g_rtCopyLinks;
bool                       g_rtCopySkipWater[ RT_MAX_COPIES ] = {};
int                        g_rtCopyUsed = 0;
std::vector< char >        g_rtPortalFilter;
std::vector< char >        g_rtHiddenSector;
double      g_rtCopyOffset[ RT_MAX_COPIES ][ 3 ] = {};
double      g_rtViewPos[ 3 ] = {};
std::vector< float > g_rtSectorSwitch;
int ( *RT_ActorRuleFlagsHook )( DCoreActor* actor ) = nullptr;
