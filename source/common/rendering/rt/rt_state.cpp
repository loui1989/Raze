// Raze RT: the state the scene code shares with the RTGL1 backend. Compiled
// with or without ray tracing support, so the scene hooks always link.

#include "rt_state.h"

FRtState rtstate;
bool     g_rtActive = false;
float    g_rtMuzzleFlash = 0.f;
float    g_rtSkyFlash    = 0.f;
bool     g_rtCameraUnderwater = false;
uint32_t    g_rtShotCounter = 0;
const char* g_rtShotEvent   = nullptr;
const void* g_rtViewerActor = nullptr;
int         g_rtViewerDrawn = 0, g_rtViewerDrawnTile = -1;
bool        g_rtViewerDrawnModel = false;
uint32_t    g_rtFrameNo = 0;
std::vector< uint32_t > g_rtSpriteStage[ 3 ];
int         g_rtViewSector  = -1;
std::vector< uint8_t > g_rtCopySector;
std::vector< RtGameLight > g_rtGameLights;
std::vector< RtSmokeSource > g_rtSmokeSources;
std::vector< RtCopyLink >  g_rtCopyLinks;
uint8_t                    g_rtCopyWater[ RT_MAX_COPIES ] = {};
std::vector< uint8_t >     g_rtWaterSide;
int                        g_rtCopyUsed = 0;
std::vector< char >        g_rtPortalFilter;
std::vector< char >        g_rtHiddenSector;
double      g_rtCopyOffset[ RT_MAX_COPIES ][ 3 ] = {};
double      g_rtViewPos[ 3 ] = {};
std::vector< float > g_rtSectorSwitch;
int ( *RT_ActorRuleFlagsHook )( DCoreActor* actor ) = nullptr;
