#pragma once

// Raze RT: what the platform code (cocoa/i_video.mm) calls. C++17, no RTGL1.

class SystemBaseFrameBuffer;

void                   RT_CreateRendererInstance( void* pMetalLayer );
void                   RT_DestroyRendererInstance();
SystemBaseFrameBuffer* RT_CreateFrameBuffer( void* hMonitor, bool fullscreen );
