// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2004-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_scene.cpp
** manages the rendering of the player's view
**
*/

#include <queue>
#include <algorithm>
#include "gi.h"
#include "build.h"
#include "v_draw.h"
//#include "a_dynlight.h"
#include "v_video.h"
#include "m_png.h"
#include "i_time.h"
#include "hw_dynlightdata.h"
#include "hw_clock.h"
#include "flatvertices.h"

#include "hw_renderstate.h"
#include "hw_lightbuffer.h"
#include "hw_cvars.h"
#include "hw_viewpointbuffer.h"
#include "hw_clipper.h"
//#include "hwrenderer/scene/hw_portal.h"
#include "hw_vrmodes.h"

#include "hw_drawstructs.h"
#include "hw_drawlist.h"
#include "hw_drawinfo.h"
#include "gamecvars.h"
#include "render.h"
#include "gamestruct.h"
#include "gamehud.h"
#include "rt_state.h"

#if HAVE_RT
void RT_SetMainCamera(const DVector3& pos, const FRotator& hwangles, float fovDeg, float ratio, float fovratio);
#endif

EXTERN_CVAR(Bool, cl_capfps)

PalEntry GlobalMapFog;
float GlobalFogDensity = 350.f;
TArray<PortalDesc> allPortals;
void Draw2D(F2DDrawer* drawer, FRenderState& state);

CVARD(Bool, hw_hightile, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "enable/disable hightile texture rendering")
bool hw_int_useindexedcolortextures;
CUSTOM_CVARD(Bool, hw_useindexedcolortextures, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "enable/disable indexed color texture rendering")
{
	if (screen) screen->SetTextureFilterMode();
}
CVARD(Bool, hw_models, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "enable/disable model rendering")



#if 0
void CollectLights(FLevelLocals* Level)
{
	IShadowMap* sm = &screen->mShadowMap;
	int lightindex = 0;

	// Todo: this should go through the blockmap in a spiral pattern around the player so that closer lights are preferred.
	for (auto light = Level->lights; light; light = light->next)
	{
		IShadowMap::LightsProcessed++;
		if (light->shadowmapped && light->IsActive() && lightindex < 1024)
		{
			IShadowMap::LightsShadowmapped++;

			light->mShadowmapIndex = lightindex;
			sm->SetLight(lightindex, (float)light->X(), (float)light->Y(), (float)light->Z(), light->GetRadius());
			lightindex++;
		}
		else
		{
			light->mShadowmapIndex = 1024;
		}

	}

	for (; lightindex < 1024; lightindex++)
	{
		sm->SetLight(lightindex, 0, 0, 0, 0);
	}
}
#endif


//-----------------------------------------------------------------------------
//
// Renders one viewpoint in a scene
//
//-----------------------------------------------------------------------------

void RenderViewpoint(FRenderViewpoint& mainvp, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
{
	auto& RenderState = *screen->RenderState();

	/*
	if (mainview && toscreen)
	{
		screen->SetAABBTree(camera->Level->aabbTree);
		screen->mShadowMap.SetCollectLights([=] {
			CollectLights(camera->Level);
		});
		screen->UpdateShadowMap();
	}
	*/

	// Render (potentially) multiple views for stereo 3d
	// Fixme. The view offsetting should be done with a static table and not require setup of the entire render state for the mode.
	auto vrmode = VRMode::GetVRMode(mainview && toscreen);
	const int eyeCount = vrmode->mEyeCount;
	screen->FirstEye();
	hw_int_useindexedcolortextures = eyeCount > 1 || RT_IsActive() ? false : *hw_useindexedcolortextures;

	// ray tracing: only the player's view goes to the path tracer (no camera
	// textures or save pictures, it has no off-screen targets)
	RT_PUSH_TYPE(rtignore, mainview && toscreen ? RtPrim::Identity : RtPrim::Ignored);
#if HAVE_RT
	if (RT_IsActive() && mainview && toscreen)
	{
		RT_SetMainCamera(mainvp.Pos, mainvp.HWAngles, fov, ratio, fovratio);
	}
#endif

	for (int eye_ix = 0; eye_ix < eyeCount; ++eye_ix)
	{
		const auto& eye = vrmode->mEyes[eye_ix];
		screen->SetViewportRects(bounds);

		if (mainview) // Bind the scene frame buffer and turn on draw buffers used by ssao
		{
			bool useSSAO = (gl_ssao != 0);
			screen->SetSceneRenderTarget(useSSAO);
			RenderState.SetPassType(useSSAO ? GBUFFER_PASS : NORMAL_PASS);
			RenderState.EnableDrawBuffers(RenderState.GetPassDrawBufferCount(), true);
		}

		auto di = HWDrawInfo::StartDrawInfo(nullptr, mainvp, nullptr);
		di->SetVisibility();
		auto& vp = di->Viewpoint;
		vp = mainvp;

		di->Set3DViewport(RenderState);
		float flash = 8.f / (r_scenebrightness + 8.f);
		di->Viewpoint.FieldOfView = FAngle::fromDeg(fov);	// Set the real FOV for the current scene (it's not necessarily the same as the global setting in r_viewpoint)

		// Stereo mode specific perspective projection
		di->VPUniforms.mProjectionMatrix = eye.GetProjection(fov, ratio, fovratio, false);

		// Stereo mode specific viewpoint adjustment
		if (eye.mShiftFactor != 0)
		{
			vp.Pos += eye.GetViewShift(vp.HWAngles.Yaw.Degrees());
			sectortype* sect = &sector[vp.SectCount];
			updatesector(DVector2(vp.Pos.X, -vp.Pos.Y), &sect);
			vp.SectCount = sectindex(sect);
		}

		di->SetupView(RenderState, vp.Pos.X, vp.Pos.Y, vp.Pos.Z, false, false);
		di->ProcessScene(toscreen);

		if (mainview)
		{
			PostProcess.Clock();
			if (toscreen) di->EndDrawScene(RenderState); // do not call this for camera textures.

			if (RenderState.GetPassType() == GBUFFER_PASS) // Turn off ssao draw buffers
			{
				RenderState.SetPassType(NORMAL_PASS);
				RenderState.EnableDrawBuffers(1);
			}

			screen->PostProcessScene(false, CM_DEFAULT, flash, []() { 
				hw_int_useindexedcolortextures = false;
				PostProcess.Unclock();
				Draw2D(&twodpsp, *screen->RenderState()); // draws the weapon sprites
				PostProcess.Clock();
				});
			PostProcess.Unclock();
		}
		di->EndDrawInfo();
		if (eyeCount - eye_ix > 1)
			screen->NextEye(eyeCount);
	}
	hw_int_useindexedcolortextures = false;
}

//===========================================================================
//
// Set up the view point.
//
//===========================================================================

FRenderViewpoint SetupViewpoint(DCoreActor* cam, const DVector3& position, int sectnum, const DRotator& angles, float fov = -1)
{
	FRenderViewpoint r_viewpoint{};
	r_viewpoint.CameraActor = cam;
	r_viewpoint.SectNums = nullptr;
	r_viewpoint.SectCount = sectnum;
	r_viewpoint.Pos = { position.X, -position.Y, -position.Z };
	r_viewpoint.HWAngles.Yaw = FAngle::fromDeg(-90. + angles.Yaw.Degrees());
	r_viewpoint.HWAngles.Pitch = FAngle::fromDeg(ClampViewPitch(angles.Pitch).Degrees());
	r_viewpoint.HWAngles.Roll = FAngle::fromDeg(angles.Roll.Degrees());
	r_viewpoint.FieldOfView = FAngle::fromDeg(fov > 0? fov :  r_fov);
	r_viewpoint.RotAngle = angles.Yaw.BAMs();
	double FocalTangent = tan(r_viewpoint.FieldOfView.Radians() / 2);
	DAngle an = DAngle::fromDeg(270. - r_viewpoint.HWAngles.Yaw.Degrees());
	r_viewpoint.TanSin = FocalTangent * an.Sin();
	r_viewpoint.TanCos = FocalTangent * an.Cos();
	r_viewpoint.ViewVector = an.ToVector();
	return r_viewpoint;
}


void DoWriteSavePic(FileWriter* file, uint8_t* scr, int width, int height, bool upsidedown)
{
	int pixelsize = 3;

	int pitch = width * pixelsize;
	if (upsidedown)
	{
		scr += ((height - 1) * width * pixelsize);
		pitch *= -1;
	}

	M_CreatePNG(file, scr, nullptr, SS_RGB, width, height, pitch, vid_gamma);
}

//===========================================================================
//
// Render the view to a savegame picture
//
// Currently a bit messy because the game side still needs to be able to
// handle Polymost.
//
//===========================================================================
bool writingsavepic;
FileWriter* savefile;
int savewidth, saveheight;

void WriteSavePic(FileWriter* file, int width, int height)
{
	writingsavepic = true;
	savefile = file;
	savewidth = width;
	saveheight = height;
	/*bool didit =*/ gi->GenerateSavePic();
	writingsavepic = false;
}

void RenderToSavePic(FRenderViewpoint& vp, FileWriter* file, int width, int height)
{
	IntRect bounds;
	bounds.left = 0;
	bounds.top = 0;
	bounds.width = width;
	bounds.height = height;
	auto& RenderState = *screen->RenderState();

	// we must be sure the GPU finished reading from the buffer before we fill it with new data.
	screen->WaitForCommands(false);

	// Switch to render buffers dimensioned for the savepic
	screen->SetSaveBuffers(true);
	screen->ImageTransitionScene(true);

	RenderState.SetVertexBuffer(screen->mVertexData);
	screen->mVertexData->Reset();
	screen->mLights->Clear();
	screen->mViewpoints->Clear();

	twodpsp.Clear();

	RenderViewpoint(vp, &bounds, vp.FieldOfView.Degrees(), 1.333f, 1.333f, true, false);


	int numpixels = width * height;
	TArray<uint8_t> scr(numpixels * 3, true);
	screen->CopyScreenToBuffer(width, height, scr.Data());

	DoWriteSavePic(file, scr.Data(), width, height, screen->FlipSavePic());

	// Switch back the screen render buffers
	screen->SetViewportRects(nullptr);
	screen->SetSaveBuffers(false);
}

//===========================================================================
//
// Renders the main view
//
//===========================================================================

static void CheckTimer(FRenderState &state, uint64_t ShaderStartTime)
{
	// if firstFrame is not yet initialized, initialize it to current time
	// if we're going to overflow a float (after ~4.6 hours, or 24 bits), re-init to regain precision
	if ((state.firstFrame == 0) || (screen->FrameTime - state.firstFrame >= 1 << 24) || ShaderStartTime >= state.firstFrame)
		state.firstFrame = screen->FrameTime;
}


void animatecamsprite(double s);


//---------------------------------------------------------------------------
//
// Ray tracing: copies of places linked to openings near the view, moved into
// place (g_rtCopySector). The path tracer has the whole map at once, so what the
// rasterizer shows through a portal must physically be there:
//  - the game's links (g_rtCopyLinks): Duke's water surfaces, the world above them
// (The map's sector portals -- Blood's rooms over rooms -- are drawn as the
// rasterizer draws them, what the opening shows: HWDrawInfo::RenderPortal.)
// Each copy keeps out anything that would cover another copy's openings.
//
//---------------------------------------------------------------------------

static void RT_BuildCopies(const DVector3& cpos, sectortype* camsect)
{
	std::vector<RtCopyLink> links = std::move(g_rtCopyLinks);
	g_rtCopyLinks.clear();
	g_rtCopySector.clear();
	g_rtCopyUsed = 0;

	constexpr double reach = 1600; // 50 m
	const unsigned n = sector.Size();

	auto centre = [](const sectortype* s)
	{
		DVector2 c(0, 0);
		for (auto& w : s->walls) c += w.pos;
		return c / max(1u, s->walls.Size());
	};

	if (links.empty()) return;

	// nearest openings first
	std::vector<double> ldist(links.size(), 1e30);
	for (size_t k = 0; k < links.size(); k++)
		for (int h : links[k].holes)
		{
			const double d = &sector[h] == camsect ? 0. : (centre(&sector[h]) - cpos.XY()).Length();
			ldist[k] = min(ldist[k], d);
		}
	std::vector<size_t> order(links.size());
	for (size_t k = 0; k < order.size(); k++) order[k] = k;
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ldist[a] < ldist[b]; });
	if (order.size() > size_t(RT_MAX_COPIES)) order.resize(RT_MAX_COPIES);

	// points inside each copy's openings
	std::vector<TArray<DVector2>> holes(order.size());
	for (size_t k = 0; k < order.size(); k++)
		for (int h : links[order[k]].holes)
		{
			auto hs = &sector[h];
			const DVector2 c = centre(hs);
			if (inside(c.X, c.Y, hs) == 1) holes[k].Push(c);
			for (auto& w : hs->walls)
			{
				const DVector2 p = w.pos * 0.9 + c * 0.1;
				if (inside(p.X, p.Y, hs) == 1) holes[k].Push(p);
			}
		}

	g_rtCopySector.assign(n, 0);
	g_rtCopyUsed = int(order.size());
	for (size_t k = 0; k < order.size(); k++)
	{
		const RtCopyLink& c = links[order[k]];
		g_rtCopyOffset[k][0] = -c.delta.X;
		g_rtCopyOffset[k][1] = -c.delta.Y;
		g_rtCopyOffset[k][2] = -c.delta.Z;
		g_rtCopySkipWater[k] = c.skipWaterFloors;

		// is any of these (opening side) points under sector 'ss' (linked side) in this copy?
		auto over = [&](const TArray<DVector2>& pts, sectortype* ss)
		{
			for (auto& p : pts)
			{
				const DVector2 q = p + c.delta.XY();
				if (inside(q.X, q.Y, ss) == 1) return true;
			}
			return false;
		};
		// nothing solid of this copy over another copy's openings (its own win)
		auto keep = [&](sectortype* ss)
		{
			if (c.skipWaterFloors && ss->lotag == 1) return true; // its floor is left out anyway
			for (size_t j = 0; j < order.size(); j++)
				if (j != k && over(holes[j], ss)) return over(holes[k], ss);
			return true;
		};
		std::vector<char> isHole(n, 0);
		for (int h : c.holes) isHole[h] = 1;

		// the linked side within reach of the camera's place there, through openings
		sectortype* start = &sector[c.start];
		updatesector(cpos.XY() + c.delta.XY(), &start);
		if (!start || isHole[sectindex(start)] || (c.skipWaterFloors && start->lotag == 2)) start = &sector[c.start];

		std::vector<double> dist(n, -1.);
		std::vector<DVector2> entry(n);
		using Item = std::pair<double, int>;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
		const int si0 = sectindex(start);
		dist[si0] = 0;
		entry[si0] = cpos.XY() + c.delta.XY();
		q.push({ 0., si0 });
		while (!q.empty())
		{
			auto [d, si] = q.top();
			q.pop();
			if (d > dist[si]) continue;
			for (auto& w : sector[si].walls)
			{
				if (!w.twoSided()) continue;
				auto ns = w.nextSector();
				if (!ns) continue;
				const int ni = sectindex(ns);
				// not back onto the opening side
				if (isHole[ni] || (c.skipWaterFloors && ns->lotag == 2)) continue;
				const DVector2 mid = (w.pos + w.point2Wall()->pos) * 0.5;
				const double nd = d + (mid - entry[si]).Length();
				if (nd > reach) continue;
				if (dist[ni] < 0 || nd < dist[ni])
				{
					dist[ni] = nd;
					entry[ni] = mid;
					q.push({ nd, ni });
				}
			}
		}
		for (unsigned i = 0; i < n; i++)
			if (dist[i] >= 0 && g_rtCopySector[i] == 0 && !isHole[i] && keep(&sector[i])) g_rtCopySector[i] = uint8_t(k + 1);
	}
}

void render_drawrooms(DCoreActor* playersprite, const DVector3& position, sectortype* sect, const DRotator& angles, double interpfrac, float fov)
{
	updatesector(position.XY(), &sect);
	if (sect == nullptr) return;
	g_rtViewSector = sectindex(sect);
	g_rtViewPos[0] = position.X; g_rtViewPos[1] = position.Y; g_rtViewPos[2] = position.Z;
	if (RT_IsActive()) RT_BuildCopies(position, sect);

	iter_dlightf = iter_dlight = draw_dlight = draw_dlightf = 0;
	checkBenchActive();

	// reset statistics counters
	ResetProfilingData();

	// Get this before everything else
	FRenderViewpoint r_viewpoint = SetupViewpoint(playersprite, position, sectindex(sect), angles, fov);
	r_viewpoint.TicFrac = !cl_capfps ? interpfrac : 1.;

	screen->mLights->Clear();
	screen->mViewpoints->Clear();
	screen->mVertexData->Reset();

	if (writingsavepic) // hack alert! The save code should not go through render_drawrooms, but we can only clean up the game side when Polymost is gone for good.
	{
		RenderToSavePic(r_viewpoint, savefile, savewidth, saveheight);
		return;
	}

	// Shader start time does not need to be handled per level. Just use the one from the camera to render from.
	auto RenderState = screen->RenderState();
	CheckTimer(*RenderState, 0/*ShaderStartTime*/);

	// prepare all camera textures that have been used in the last frame.
	gi->UpdateCameras(r_viewpoint.TicFrac);

	RenderState->SetVertexBuffer(screen->mVertexData);

	// now render the main view
	float fovratio;
	float ratio = ActiveRatio(screen->GetWidth(), screen->GetHeight());
	if (ratio >= 1.33f)
	{
		fovratio = 1.33f;
	}
	else
	{
		fovratio = ratio;
	}

	screen->ImageTransitionScene(true); // Only relevant for Vulkan.

	RenderViewpoint(r_viewpoint, nullptr, r_viewpoint.FieldOfView.Degrees(), ratio, fovratio, true, true);
	All.Unclock();
}

void render_camtex(DCoreActor* playersprite, const DVector3& position, sectortype* sect, const DRotator& angles, FGameTexture* camtex, IntRect& rect, double interpfrac)
{
	updatesector(position, &sect);
	if (!sect) return;

	screen->RenderState()->SetVertexBuffer(screen->mVertexData);

	// now render the main view
	float ratio = camtex->GetDisplayWidth() / camtex->GetDisplayHeight();

	FRenderViewpoint r_viewpoint = SetupViewpoint(playersprite, position, sectindex(sect), angles);
	r_viewpoint.TicFrac = !cl_capfps ? interpfrac : 1.;

	RenderViewpoint(r_viewpoint, &rect, r_viewpoint.FieldOfView.Degrees(), ratio, ratio, false, false);
	All.Unclock();
}

FSerializer& Serialize(FSerializer& arc, const char* key, PortalDesc& obj, PortalDesc* defval)
{
	if (arc.BeginObject(key))
	{
		arc("type", obj.type)
			("d", obj.delta)
			("targets", obj.targets)
			.EndObject();
	}
	return arc;
}
