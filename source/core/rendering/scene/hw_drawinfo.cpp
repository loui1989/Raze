// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2000-2018 Christoph Oelckers
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
** gl_drawinfo.cpp
** Basic scene draw info management class
**
*/

#include "hw_portal.h"
#include "build.h"
#include "hw_renderstate.h"
#include "hw_drawinfo.h"
#include "rt_state.h"
#include "models/modeldata.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_vrmodes.h"
#include "hw_clipper.h"
#include "v_draw.h"
#include "gamecvars.h"
#include "gamestruct.h"
#include "gamecontrol.h"
#include "mapinfo.h"
#include <queue>
#include "automap.h"
#include "hw_voxels.h"
#include "coreactor.h"
#include "tiletexture.h"

#include "buildtiles.h"

EXTERN_CVAR(Float, r_visibility)
CVAR(Bool, gl_no_skyclear, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// ray tracing: voxel models further than this (metres) are drawn as their sprites --
// every voxel instance is a big mesh RTGL1 rebuilds each frame. Duke's, and in the
// other games a voxel pack's: the games' own voxels (Blood's) are small, and their
// sprites are not made to stand in (gravestones float)
CVARD(Bool, rt_portal_views, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: draw what room-over-room openings show (Blood)")
CVARD(Float, rt_voxel_distance, 12.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: voxel models further than this many metres are drawn as flat sprites (0 = no voxels)")

CVAR(Bool, gl_texture, true, 0)
CVAR(Float, gl_mask_threshold, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_mask_sprite_threshold, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

BitArray gotsector;

CVARD(Bool, rt_voxel_insight, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: voxel models in sight are models at any distance (rt_voxel_distance is then for those only reflections and shadows see)")

bool RT_VoxelTooFar(int vox, const tspritetype* spr, const DVector3& view)
{
	if (vox < 0 || !RT_IsActive()) return false;
	if (!isDuke() && !voxdefined[vox]) return false;
	if (*rt_voxel_insight && !spr->rtoffscreen) return false;
	const double far = double(*rt_voxel_distance) * 32.;
	return (spr->pos - view).LengthSquared() > far * far;
}

bool VoxelTopZ(tspritetype* spr, double& top)
{
	auto actor = spr->ownerActor;
	const int vox = GetExtInfo(spr->spritetexture()).tiletovox;
	if (!r_voxels || vox < 0 || !voxmodels[vox] || !actor) return false;
	if ((actor->sprext.renderflags & SPREXT_NOTMD) || (spr->cstat2 & CSTAT2_SPRITE_NOMODEL)) return false;
	if ((spr->cstat & CSTAT_SPRITE_YFLIP) || (spr->cstat & CSTAT_SPRITE_ALIGNMENT_MASK) == CSTAT_SPRITE_ALIGNMENT_FLOOR) return false;
	if (RT_VoxelTooFar(vox, spr, DVector3(g_rtViewPos[0], g_rtViewPos[1], g_rtViewPos[2]))) return false;

	const voxmodel_t* voxel = voxmodels[vox];
	const double sprz = spr->scale.Y * voxel->bscale;
	const double zpos = spr->pos.Z + actor->sprext.position_offset.Z - spr->yoffset * sprz;
	double zoff = voxel->siz.Z * .5;
	if (!(spr->cstat & CSTAT_SPRITE_YCENTER))
		zoff += voxel->piv.Z;
	else if ((spr->cstat & CSTAT_SPRITE_ALIGNMENT_MASK) != CSTAT_SPRITE_ALIGNMENT_SLAB)
		zoff += voxel->piv.Z - voxel->siz.Z * .5;
	top = zpos + voxel->zadd * voxel->scale * sprz - voxel->scale * sprz * zoff;
	return true;
}

static void RT_PlanPortalViews(HWDrawInfo* di, unsigned seen);

//==========================================================================
//
//
//
//==========================================================================

class FDrawInfoList
{
public:
	TDeletingArray<HWDrawInfo *> mList;

	HWDrawInfo * GetNew();
	void Release(HWDrawInfo *);
};


FDrawInfoList di_list;

//==========================================================================
//
// Try to reuse the lists as often as possible as they contain resources that
// are expensive to create and delete.
//
// Note: If multithreading gets used, this class needs synchronization.
//
//==========================================================================

HWDrawInfo *FDrawInfoList::GetNew()
{
	if (mList.Size() > 0)
	{
		HWDrawInfo *di;
		mList.Pop(di);
		return di;
	}
	return new HWDrawInfo();
}

void FDrawInfoList::Release(HWDrawInfo * di)
{
	di->ClearBuffers();
	mList.Push(di);
}

//==========================================================================
//
// Sets up a new drawinfo struct
//
//==========================================================================

HWDrawInfo *HWDrawInfo::StartDrawInfo(HWDrawInfo *parent, FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms)
{
	HWDrawInfo *di = di_list.GetNew();
	di->StartScene(parentvp, uniforms);
	return di;
}


//==========================================================================
//
//
//
//==========================================================================

static Clipper staticClipper;		// Since all scenes are processed sequentially we only need one clipper.
static HWDrawInfo * gl_drawinfo;	// This is a linked list of all active DrawInfos and needed to free the memory arena after the last one goes out of scope.

void HWDrawInfo::StartScene(FRenderViewpoint& parentvp, HWViewpointUniforms* uniforms)
{
	mClipper = &staticClipper;

	Viewpoint = parentvp;
	//lightmode = Level->lightMode;
	if (uniforms)
	{
		VPUniforms = *uniforms;
		// The clip planes will never be inherited from the parent drawinfo.
		VPUniforms.mClipLine.X = -1000001.f;
		VPUniforms.mClipHeight = 0;
	}
	else
	{
		VPUniforms.mProjectionMatrix.loadIdentity();
		VPUniforms.mViewMatrix.loadIdentity();
		VPUniforms.mNormalViewMatrix.loadIdentity();
		//VPUniforms.mViewHeight = viewheight;
		VPUniforms.mGlobVis = (2 / 65536.f) * (g_visibility + g_relvisibility) / r_ambientlight;
		VPUniforms.mPalLightLevels = numshades | (static_cast<int>(gl_fogmode) << 8);
		if (isBuildSoftwareLighting()) VPUniforms.mPalLightLevels |= (5 << 16);

		VPUniforms.mClipLine.X = -10000000.0f;
		VPUniforms.mShadowmapFilter = gl_shadowmap_filter;
	}

	ClearBuffers();

	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	vpIndex = 0;

	// Fullbright information needs to be propagated from the main view.
	if (outer != nullptr) FullbrightFlags = outer->FullbrightFlags;
	else FullbrightFlags = 0;

	outer = gl_drawinfo;
	gl_drawinfo = this;

}

//==========================================================================
//
//
//
//==========================================================================

HWDrawInfo *HWDrawInfo::EndDrawInfo()
{
	assert(this == gl_drawinfo);
	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	gl_drawinfo = outer;
	di_list.Release(this);
	if (gl_drawinfo == nullptr)
		ResetRenderDataAllocator();
	return gl_drawinfo;
}


//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::ClearBuffers()
{
	spriteindex = 0;
	mClipPortal = nullptr;
	mCurrentPortal = nullptr;
}

//-----------------------------------------------------------------------------
//
// R_FrustumAngle
//
//-----------------------------------------------------------------------------

angle_t HWDrawInfo::FrustumAngle()
{
	float WidescreenRatio = (float)screen->GetWidth() / screen->GetHeight();
	float tilt = fabs(Viewpoint.HWAngles.Pitch.Degrees());

	// If the pitch is larger than this you can look all around at a FOV of 90°
	if (tilt > 46.0f) return 0xffffffff;

	// ok, this is a gross hack that barely works...
	// but at least it doesn't overestimate too much...
	double floatangle = 2.0 + (45.0 + ((tilt / 1.9)))*Viewpoint.FieldOfView.Degrees() * 48.0 / AspectMultiplier(WidescreenRatio) / 90.0;
	angle_t a1 = DAngle::fromDeg(floatangle).BAMs();
	if (a1 >= ANGLE_90) return 0xffffffff; // it's either below 90 or bust.
	return a1;
}

//-----------------------------------------------------------------------------
//
// Setup the modelview matrix
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetViewMatrix(const FRotator &angles, float vx, float vy, float vz, bool mirror, bool planemirror)
{
	float mult = mirror ? -1.f : 1.f;
	float planemult = planemirror ? -1 : 1;// Level->info->pixelstretch : Level->info->pixelstretch;

	VPUniforms.mViewMatrix.loadIdentity();
	VPUniforms.mViewMatrix.rotate(angles.Roll.Degrees(), 0.0f, 0.0f, 1.0f);
	VPUniforms.mViewMatrix.rotate(angles.Pitch.Degrees(), 1.0f, 0.0f, 0.0f);
	VPUniforms.mViewMatrix.rotate(angles.Yaw.Degrees(), 0.0f, mult, 0.0f);
	VPUniforms.mViewMatrix.translate(vx * mult, -vz * planemult, -vy);
	VPUniforms.mViewMatrix.scale(-mult, planemult, 1);
}


//-----------------------------------------------------------------------------
//
// SetupView
// Setup the view rotation matrix for the given viewpoint
//
//-----------------------------------------------------------------------------
void HWDrawInfo::SetupView(FRenderState &state, float vx, float vy, float vz, bool mirror, bool planemirror)
{
	auto &vp = Viewpoint;
	//vp.SetViewAngle(r_viewwindow); // todo: need to pass in.
	SetViewMatrix(vp.HWAngles, vx, vy, vz, mirror, planemirror);
	SetCameraPos(vp.Pos);
	VPUniforms.CalcDependencies();
	VPUniforms.mLightBlendMode = 0;
	vpIndex = screen->mViewpoints->SetViewpoint(state, &VPUniforms);
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

HWPortal * HWDrawInfo::FindPortal(const void * src)
{
	int i = Portals.Size() - 1;

	while (i >= 0 && Portals[i] && Portals[i]->GetSource() != src) i--;
	return i >= 0 ? Portals[i] : nullptr;
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

void HWDrawInfo::DispatchSprites()
{
	for (unsigned i = 0; i < tsprites.Size(); i++)
	{
		auto tspr = tsprites.get(i);
		auto actor = tspr->ownerActor;
		auto texid = tspr->spritetexture();

		if (actor == nullptr || tspr->scale.X == 0 || tspr->scale.Y == 0 || !texid.isValid()) continue;

		// ray tracing: a sprite entirely under its sector's floor is out of sight in the game
		// (Build clips sprites at the floors they are seen over: E4M1 keeps its lightning bolts
		// down there), but to the path tracer it is real geometry, seen through water above it
		if (RT_IsActive() && tspr->sectp && !tspr->rtcopy && (tspr->cstat & CSTAT_SPRITE_ALIGNMENT_MASK) != CSTAT_SPRITE_ALIGNMENT_FLOOR &&
			!(tspr->sectp->floorstat & CSTAT_SECTOR_SKY))
		{
			auto tex = TexMan.GetGameTexture(texid);
			const double h = tex ? tex->GetDisplayHeight() * tspr->scale.Y : 0;
			const double top = (tspr->cstat & CSTAT_SPRITE_YCENTER) ? tspr->pos.Z - h * 0.5 : tspr->pos.Z - h;
			if (h > 0 && top > getflorzofslopeptr(tspr->sectp, tspr->pos.X, tspr->pos.Y) + 1) continue;
		}

		if (i < rtHiddenSpriteStart) actor->spr.cstat2 |= CSTAT2_SPRITE_MAPPED;	// not for what only the path tracer sees

		if (!(tspr->cstat2 & CSTAT2_SPRITE_NOANIMATE))
		{
			tileUpdatePicnum(texid, (actor->GetIndex() & 16383));
		}
		if (tspr->cstat2 & CSTAT2_SPRITE_FULLBRIGHT)
			tspr->shade = -127;
		tspr->setspritetexture(texid);

		if (!(actor->sprext.renderflags & SPREXT_NOTMD) && !(tspr->cstat2 & CSTAT2_SPRITE_NOMODEL))
		{
			auto pt = modelManager.GetModel(tspr->spritetexture(), tspr->pal);
			if (hw_models && pt && pt->modelid >= 0 && pt->framenum >= 0)
			{
				//HWSprite hwsprite;
				//if (hwsprite.ProcessModel(pt, tspr)) continue;
			}
			if (r_voxels)
			{
				auto vox = GetExtInfo(texid).tiletovox;
				if (vox >= 0 && voxmodels[vox])
				{
					if (RT_VoxelTooFar(vox, tspr, DVector3(g_rtViewPos[0], g_rtViewPos[1], g_rtViewPos[2]))) vox = -1;
					// a sprite with a glowmap of its own (Duke-RT's neon signs): the sprite, glowing
					// as drawn -- the voxel model has only the palette's fullbright colours to glow
					if (vox >= 0 && RT_IsActive())
					{
						auto gt = TexMan.GetGameTexture(texid);
						if (gt && gt->GetGlowmap()) vox = -1;
					}
				}
				if (vox >= 0 && voxmodels[vox])
				{
					HWSprite hwsprite;
					if (hwsprite.ProcessVoxel(this, voxmodels[vox], tspr, tspr->sectp, voxrotate[vox])) 
						continue;
				}
			}
		}

		if (actor->sprext.renderflags & SPREXT_AWAY1)
		{
			tspr->pos += tspr->Angles.Yaw.ToVector() * 0.125;
		}
		else if (actor->sprext.renderflags & SPREXT_AWAY2)
		{
			tspr->pos -= tspr->Angles.Yaw.ToVector() * 0.125;
		}

		switch (tspr->cstat & CSTAT_SPRITE_ALIGNMENT_MASK)
		{
		case CSTAT_SPRITE_ALIGNMENT_FACING:
		{
			HWSprite sprite;
			sprite.Process(this, tspr, tspr->sectp, false);
			break;
		}

		case CSTAT_SPRITE_ALIGNMENT_WALL:
		{
			HWWall wall;
			wall.ProcessWallSprite(this, tspr, tspr->sectp);
			break;
		}

		case CSTAT_SPRITE_ALIGNMENT_FLOOR:
		{
			HWFlat flat;
			flat.ProcessFlatSprite(this, tspr, tspr->sectp);
			break;
		}

		default:
			break;
		}
	}
}
//-----------------------------------------------------------------------------
//
// CreateScene
//
// creates the draw lists for the current scene
//
//-----------------------------------------------------------------------------

void HWDrawInfo::CreateScene(bool portal)
{
	const auto& vp = Viewpoint;

	angle_t a1 = FrustumAngle();

	// reset the portal manager
	portalState.StartFrame();

	ProcessAll.Clock();

	// clip the scene and fill the drawlists
	screen->mVertexData->Map();
	screen->mLights->Map();

	tsprites.clear();
	rtHiddenSpriteStart = ~0u;
	ingeo = false;
	geoofs = { 0,0 };

	if(!portal) mClipper->SetVisibleRange(vp.RotAngle, a1);

	if (a1 != 0xffffffff) mDrawer.Init(this, mClipper, vp.Pos.XY(), vp.RotAngle - a1, vp.RotAngle + a1);
	else mDrawer.Init(this, mClipper, vp.Pos.XY(), 0, 0);
	if (vp.SectNums)
		mDrawer.RenderScene(vp.SectNums, vp.SectCount, portal);
	else
		mDrawer.RenderScene(&vp.SectCount, 1, portal);

	// the path tracer needs what is off screen too: in the main view, all of the map;
	// in a room-over-room view, all of the linked place it may show (the clipper only
	// gets what is seen through the opening from here)
	const bool rtMain = RT_IsActive() && mCurrentPortal == nullptr;
	const bool rtCopyView = RT_IsActive() && mCurrentPortal != nullptr && !g_rtPortalFilter.empty();
	if (rtMain || rtCopyView)
	{
		const unsigned seen = Portals.Size();
		if (rtMain) RT_PlanPortalViews(this, seen);
		rtHiddenSpriteStart = tsprites.Size();
		mDrawer.RT_ProcessRemaining(rtCopyView);
		// openings found there are not seen: no views through them
		for (unsigned i = seen; i < Portals.Size(); i++) Portals[i]->rtOffscreen = true;
		if (rtMain) mDrawer.RT_ProcessCopy();
	}

	// (the ones only the path tracer needs: their voxel models only as near as rt_voxel_distance)
	for (unsigned i = rtHiddenSpriteStart; i < tsprites.Size(); i++) tsprites.get(i)->rtoffscreen = 1;

	SetupSprite.Clock();
	// vp is in render space, so we must convert back.
	gi->processSprites(tsprites, DVector3(vp.Pos.X, -vp.Pos.Y, -vp.Pos.Z), DAngle::fromBam(vp.RotAngle), vp.TicFrac);
	// ray tracing: the copied sprites go where their copied area is (after the game
	// has animated them where they really are)
	if (rtMain && !g_rtCopySector.empty())
	{
		for (unsigned i = 0; i < tsprites.Size(); i++)
		{
			auto t = tsprites.get(i);
			if (!t->rtcopy) continue;
			const double* ofs = RT_CopyOffset(t->rtcopy);
			t->pos += DVector3(ofs[0], ofs[1], ofs[2]);
		}
	}
	DispatchSprites();
	SetupSprite.Unclock();

	GeoEffect eff;
	int effsect = vp.SectNums ? vp.SectNums[0] : vp.SectCount;
	auto drawsectp = &sector[effsect];
	auto orgdrawsectp = drawsectp;
	// RR geometry hack. Ugh...
	// This just adds to the existing render list, so we must offset the effect areas to the same xy-space as the main one as we cannot change the view matrix.
	if (gi->GetGeoEffect(&eff, drawsectp))
	{
		ingeo = true;
		geoofs = { (float)eff.geox[0], (float)eff.geoy[0] };
		// process the first layer.
		for (int i = 0; i < eff.geocnt; i++)
		{
			auto sect = eff.geosectorwarp[i];
			for (unsigned w = 0; w < sect->walls.Size(); w++)
			{
				auto wal = &sect->walls[w];
				wal->pos.X += eff.geox[i];
				wal->pos.Y += eff.geoy[i];
			}
			sect->dirty = EDirty::AllDirty;
			if (eff.geosector[i] == drawsectp) drawsectp = eff.geosectorwarp[i];
		}

		if (a1 != 0xffffffff) mDrawer.Init(this, mClipper, vp.Pos.XY(), vp.RotAngle - a1, vp.RotAngle + a1);
		else mDrawer.Init(this, mClipper, vp.Pos.XY(), 0, 0);

		int drawsect = sectindex(drawsectp);
		mDrawer.RenderScene(&drawsect, 1, false);

		for (int i = 0; i < eff.geocnt; i++)
		{
			auto sect = eff.geosectorwarp[i];
			for (unsigned w = 0; w < sect->walls.Size(); w++)
			{
				auto wal = &sect->walls[w];
				wal->pos.X -= eff.geox[i];
				wal->pos.Y -= eff.geoy[i];
			}
		}

		// Now the second layer. Same shit, different arrays.
		geoofs = { (float)eff.geox2[0], (float)eff.geoy2[0] };
		for (int i = 0; i < eff.geocnt; i++)
		{
			auto sect = eff.geosectorwarp2[i];
			for (unsigned w = 0; w < sect->walls.Size(); w++)
			{
				auto wal = &sect->walls[w];
				wal->pos.X += eff.geox2[i];
				wal->pos.Y += eff.geoy2[i];
			}
			sect->dirty = EDirty::AllDirty;
			if (eff.geosector[i] == orgdrawsectp) drawsectp = eff.geosectorwarp2[i];
		}

		if (a1 != 0xffffffff) mDrawer.Init(this, mClipper, vp.Pos.XY(), vp.RotAngle - a1, vp.RotAngle + a1);
		else mDrawer.Init(this, mClipper, vp.Pos.XY(), 0, 0);
		drawsect = sectindex(drawsectp);
		mDrawer.RenderScene(&drawsect, 1, false);

		for (int i = 0; i < eff.geocnt; i++)
		{
			auto sect = eff.geosectorwarp2[i];
			for (unsigned w = 0; w < sect->walls.Size(); w++)
			{
				auto wal = &sect->walls[w];
				wal->pos.X -= eff.geox2[i];
				wal->pos.Y -= eff.geoy2[i];
			}
		}
		ingeo = false;
	}


	screen->mLights->Unmap();
	screen->mVertexData->Unmap();

	ProcessAll.Unclock();

}

//-----------------------------------------------------------------------------
//
// RenderScene
//
// Draws the current draw lists for the non GLSL renderer
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderScene(FRenderState &state)
{
	RenderAll.Clock();

	state.SetDepthMask(true);

	state.EnableFog(true);
	state.SetRenderStyle(STYLE_Source);


	// Part 1: solid geometry. This is set up so that there are no transparent parts
	state.SetDepthFunc(DF_Less);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.ClearDepthBias();

	state.EnableTexture(gl_texture);
	state.EnableBrightmap(true);
	drawlists[GLDL_PLAINWALLS].DrawWalls(this, state, false);

	drawlists[GLDL_PLAINFLATS].DrawFlats(this, state, false);


	// Part 2: masked geometry. This is set up so that only pixels with alpha>gl_mask_threshold will show
	state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);

	// This list is masked, non-translucent walls.
	drawlists[GLDL_MASKEDWALLS].DrawWalls(this, state, false);

	// These lists must be drawn in two passes for color and depth to avoid depth fighting with overlapping entries
	drawlists[GLDL_MASKEDFLATS].SortFlats(this);
	drawlists[GLDL_MASKEDWALLSV].SortWallsHorz(this);
	drawlists[GLDL_MASKEDWALLSH].SortWallsVert(this);
	drawlists[GLDL_MASKEDWALLSD].SortWallsDiag(this);


	// these lists are only wall and floor sprites - often attached to walls and floors - so they need to be offset from the plane they may be attached to.
	drawlists[GLDL_MASKEDWALLSS].DrawWalls(this, state, false);

	// Each list must draw both its passes before the next one to ensure proper depth buffer contents.
	auto& list = drawlists[GLDL_MASKEDWALLSD].drawitems;
	unsigned i = 0;
	RenderWall.Clock();
	while (i < list.Size())
	{
		unsigned j;
		auto check = drawlists[GLDL_MASKEDWALLSD].walls[list[i].index]->walldist;
		state.SetDepthMask(false);
		for (j = i; j < list.Size() && drawlists[GLDL_MASKEDWALLSD].walls[list[j].index]->walldist == check; j++)
		{
			drawlists[GLDL_MASKEDWALLSD].walls[list[j].index]->DrawWall(this, state, false);
		}
		state.SetDepthMask(true);
		for (unsigned k = i; k < j; k++)
		{
			drawlists[GLDL_MASKEDWALLSD].walls[list[k].index]->DrawWall(this, state, false);
		}
		i = j;
	}
	RenderWall.Unclock();

	state.SetDepthMask(false);
	drawlists[GLDL_MASKEDWALLSD].DrawWalls(this, state, false);
	state.SetDepthMask(true);
	state.SetColorMask(false);
	drawlists[GLDL_MASKEDWALLSD].DrawWalls(this, state, false);
	state.SetColorMask(true);

	state.SetDepthMask(false);
	drawlists[GLDL_MASKEDWALLSV].DrawWalls(this, state, false);
	state.SetDepthMask(true);
	state.SetColorMask(false);
	drawlists[GLDL_MASKEDWALLSV].DrawWalls(this, state, false);
	state.SetColorMask(true);

	state.SetDepthMask(false);
	drawlists[GLDL_MASKEDWALLSH].DrawWalls(this, state, false);
	state.SetDepthMask(true);
	state.SetColorMask(false);
	drawlists[GLDL_MASKEDWALLSH].DrawWalls(this, state, false);
	state.SetColorMask(true);

	state.SetDepthBias(-1, -128);
	drawlists[GLDL_MASKEDSLOPEFLATS].DrawFlats(this, state, false);
	state.SetDepthMask(false);
	drawlists[GLDL_MASKEDFLATS].DrawFlats(this, state, false);
	state.SetDepthMask(true);
	state.SetColorMask(false);
	drawlists[GLDL_MASKEDFLATS].DrawFlats(this, state, false);
	state.SetColorMask(true);
	state.ClearDepthBias();

	drawlists[GLDL_MODELS].Draw(this, state, false);

	state.SetRenderStyle(STYLE_Translucent);

	state.SetDepthFunc(DF_LEqual);
	RenderAll.Unclock();
}

//-----------------------------------------------------------------------------
//
// RenderTranslucent
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderTranslucent(FRenderState &state)
{
	RenderAll.Clock();

	state.SetDepthBias(-1, -160);

	// final pass: translucent stuff
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.SetRenderStyle(STYLE_Translucent);

	state.EnableBrightmap(true);
	drawlists[GLDL_TRANSLUCENTBORDER].Draw(this, state, true);
	state.SetDepthMask(false);

	drawlists[GLDL_TRANSLUCENT].DrawSorted(this, state);
	state.EnableBrightmap(false);

	state.ClearDepthBias();
	state.AlphaFunc(Alpha_GEqual, 0.5f);
	state.SetDepthMask(true);

	RenderAll.Unclock();
}


//-----------------------------------------------------------------------------
//
// RenderTranslucent
//
//-----------------------------------------------------------------------------

//==========================================================================
//
// Ray tracing: rooms over rooms (Blood). The rasterizer draws what an opening
// shows as a view of the linked place, stencilled to the opening. The path tracer
// has no stencil: the linked place is drawn moved to where it is seen (a copy,
// g_rtCopyOffset), and only where it fills space that is empty around the viewer.
// Where the moved place would overlap real sectors in 3D:
//  - real sectors the view sees, or in the viewer's surroundings: that part of the
//    copy is left out (the courtyard copied over itself: enemies knee deep in it, a
//    second mortuary door)
//  - otherwise the real sectors are left out instead and the copy drawn there (the
//    crypt over the coffin sits partly on the real crypt, which the coffin cannot
//    see; the mortuary's upper storey is stacked right over the courtyard, a
//    little off from where the opening shows it)
//
//==========================================================================

CVARD(Float, rt_portal_near, 24.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: room-over-room views: real rooms within this many metres' walk count as the viewer's surroundings")

// per sector, its bounding box (min x, min y, max x, max y) and some points inside it
// -- for telling where sectors overlap. Made once per level.
static std::vector<DVector4> rt_sectorBox;
static std::vector<std::vector<DVector2>> rt_sectorPoints;
static void RT_SectorShapes()
{
	static const void* level = nullptr;
	static const void* data = nullptr;
	if (level == currentLevel && data == sector.Data() && rt_sectorBox.size() == sector.Size()) return;
	level = currentLevel; data = sector.Data();
	const unsigned n = sector.Size();
	rt_sectorBox.assign(n, DVector4(0, 0, 0, 0));
	rt_sectorPoints.assign(n, {});
	for (unsigned i = 0; i < n; i++)
	{
		auto& sec = sector[i];
		DVector4 b(1e30, 1e30, -1e30, -1e30);
		DVector2 c(0, 0);
		for (auto& w : sec.walls)
		{
			b.X = min(b.X, w.pos.X); b.Y = min(b.Y, w.pos.Y);
			b.Z = max(b.Z, w.pos.X); b.W = max(b.W, w.pos.Y);
			c += w.pos;
		}
		rt_sectorBox[i] = b;
		if (sec.walls.Size() == 0) continue;
		c /= double(sec.walls.Size());
		if (inside(c.X, c.Y, &sec) == 1) rt_sectorPoints[i].push_back(c);
		for (auto& w : sec.walls)
		{
			for (double t : { 0.1, 0.4 })
			{
				const DVector2 pt = w.pos * (1.0 - t) + c * t;
				if (inside(pt.X, pt.Y, &sec) == 1) rt_sectorPoints[i].push_back(pt);
			}
			// just inside the middle of each wall (thin sectors: door frames, ledges)
			const DVector2 a = w.pos, e = w.point2Wall()->pos, d = e - a;
			const double len = d.Length();
			if (len < 1.0) continue;
			for (double side : { 1.0, -1.0 })
			{
				const DVector2 pt = (a + e) * 0.5 + DVector2(-d.Y, d.X) * (side * 1.0 / len);
				if (inside(pt.X, pt.Y, &sec) == 1) { rt_sectorPoints[i].push_back(pt); break; }
			}
		}
	}
}

// the linked place near the opening: the sectors within ~30 m of the portal's
// targets, through any openings (shut doors too: they open)
static std::vector<char> RT_PortalCandidates(const PortalDesc* pd)
{
	const unsigned n = sector.Size();
	std::vector<char> cand(n, 0);
	std::vector<double> dist(n, 1e30);
	std::vector<DVector2> entry(n);
	using Item = std::pair<double, int>;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
	for (int t : pd->targets)
	{
		if (unsigned(t) >= n) continue;
		DVector2 c(0, 0);
		for (auto& w : sector[t].walls) c += w.pos;
		entry[t] = c / max(1u, sector[t].walls.Size());
		dist[t] = 0;
		q.push({ 0., t });
	}
	while (!q.empty())
	{
		auto [d, si] = q.top();
		q.pop();
		if (d > dist[si]) continue;
		cand[si] = 1;
		for (auto& w : sector[si].walls)
		{
			if (!w.twoSided()) continue;
			const int ni = w.nextsector;
			if (unsigned(ni) >= n) continue;
			const DVector2 mid = (w.pos + w.point2Wall()->pos) * 0.5;
			const double nd = d + (mid - entry[si]).Length();
			if (nd > 960 || nd >= dist[ni]) continue;
			dist[ni] = nd; entry[ni] = mid; q.push({ nd, ni });
		}
	}
	return cand;
}

// the real sectors that sector 'ci', moved by 'shift', would overlap in 3D
static void RT_CopyClashes(unsigned ci, const DVector3& shift, std::vector<int>& out)
{
	out.clear();
	const unsigned n = sector.Size();
	auto& ls = sector[ci];
	const auto& lb = rt_sectorBox[ci];
	const double z0 = ls.ceilingz + shift.Z, z1 = ls.floorz + shift.Z;
	for (unsigned ri = 0; ri < n; ri++)
	{
		const auto& rb = rt_sectorBox[ri];
		if (lb.X + shift.X >= rb.Z || rb.X >= lb.Z + shift.X || lb.Y + shift.Y >= rb.W || rb.Y >= lb.W + shift.Y) continue;
		auto& rs = sector[ri];
		if (!(z0 < rs.floorz - 1.0 && rs.ceilingz < z1 - 1.0)) continue;	// heights apart
		bool clash = false;
		for (auto& p : rt_sectorPoints[ci])
			if (inside(p.X + shift.X, p.Y + shift.Y, &rs) == 1) { clash = true; break; }
		if (!clash)
			for (auto& p : rt_sectorPoints[ri])
				if (inside(p.X - shift.X, p.Y - shift.Y, &ls) == 1) { clash = true; break; }
		if (clash) out.push_back(int(ri));
	}
}

// walking distance from the view to each sector, through any openings (map units)
static std::vector<double> RT_ViewDistance()
{
	const unsigned n = sector.Size();
	std::vector<double> dist(n, 1e30);
	const int cs = g_rtViewSector;
	if (unsigned(cs) >= n) return dist;
	std::vector<DVector2> entry(n);
	using Item = std::pair<double, int>;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
	dist[cs] = 0; entry[cs] = DVector2(g_rtViewPos[0], g_rtViewPos[1]);
	q.push({ 0., cs });
	while (!q.empty())
	{
		auto [d, si] = q.top();
		q.pop();
		if (d > dist[si]) continue;
		for (auto& w : sector[si].walls)
		{
			if (!w.twoSided()) continue;
			const int ni = w.nextsector;
			if (unsigned(ni) >= n) continue;
			const DVector2 mid = (w.pos + w.point2Wall()->pos) * 0.5;
			const double nd = d + (mid - entry[si]).Length();
			if (nd >= dist[ni]) continue;
			dist[ni] = nd; entry[ni] = mid; q.push({ nd, ni });
		}
	}
	return dist;
}

// whether the opening can be seen from 'cam' (the camera, in the frame of the place
// the opening is in): a floor's from above it, a ceiling's from below
static bool RT_OpeningFaces(const PortalDesc* pd, int type, const DVector3& cam)
{
	const int pnum = int(pd - allPortals.Data());
	bool found = false;
	for (auto& os : sector)
	{
		if (os.portalnum != pnum || os.portalflags != type) continue;
		found = true;
		if (type == PORTAL_SECTOR_FLOOR ? cam.Z < os.floorz + 1.0 : cam.Z > os.ceilingz - 1.0) return true;
	}
	return !found;
}

struct RtPortalPlan
{
	const PortalDesc* pd;
	std::vector<char> allow;	// empty: no view
	unsigned candidates, nearClash, hidden;
};
static std::vector<RtPortalPlan> rt_portalPlans;

// the openings the view sees (found by the clipper): which part of each linked place
// is drawn, and which real sectors are left out for it (g_rtHiddenSector). Before the
// draw lists are drawn: that is where the real sectors are left out.
static void RT_PlanPortalViews(HWDrawInfo* di, unsigned seen)
{
	const unsigned n = sector.Size();
	g_rtHiddenSector.assign(n, 0);
	rt_portalPlans.clear();
	if (!rt_portal_views) return;

	std::vector<double> near;
	std::vector<int> clashes;
	const DVector3 cam(g_rtViewPos[0], g_rtViewPos[1], g_rtViewPos[2]);
	const double nearDist = max(0.f, *rt_portal_near) * 32.0;	// 32 map units a metre
	for (unsigned i = 0; i < seen && i < di->Portals.Size(); i++)
	{
		HWPortal* gp = di->Portals[i];
		const int type = gp->GetType();
		if (type != PORTAL_SECTOR_FLOOR && type != PORTAL_SECTOR_CEILING) continue;
		const PortalDesc* pd = static_cast<HWSectorStackPortal*>(gp)->GetPortalDesc();
		if (!pd) continue;
		bool dup = false;
		for (auto& pl : rt_portalPlans) dup |= pl.pd == pd;
		if (dup) continue;

		RtPortalPlan plan{ pd, {}, 0, 0, 0 };
		if (RT_OpeningFaces(pd, type, cam))
		{
			if (near.empty()) { RT_SectorShapes(); near = RT_ViewDistance(); }
			const std::vector<char> cand = RT_PortalCandidates(pd);
			const DVector3 shift = -pd->delta;
			// a real sector the copy may stand in for: one the view does not see (the
			// clipper did not get it) and not in the viewer's surroundings. (Seen from
			// the coffin, the real crypt; from the courtyard, the mortuary's upper storey
			// stacked over it. Not the crypt's corridors, seen from the crypt while the
			// courtyard's sky shows through a door.)
			const BitArray& got = di->mDrawer.GotSector();
			auto mayHide = [&](int r)
			{
				return near[r] >= nearDist && !got[r];
			};
			plan.allow.assign(n, 0);
			std::vector<int> hide;
			for (unsigned c = 0; c < n; c++)
			{
				if (!cand[c]) continue;
				plan.candidates++;
				RT_CopyClashes(c, shift, clashes);
				bool keepReal = false;
				for (int r : clashes) keepReal |= !mayHide(r);
				if (keepReal) { plan.nearClash++; continue; }
				plan.allow[c] = 1;
				hide.insert(hide.end(), clashes.begin(), clashes.end());
			}
			for (int r : hide)
			{
				if (!g_rtHiddenSector[r]) plan.hidden++;
				g_rtHiddenSector[r] = 1;
			}
		}
		rt_portalPlans.push_back(std::move(plan));
	}
}

CCMD(rt_portal_info)
{
	Printf("room-over-room views: %u (view in sector %d)\n", unsigned(rt_portalPlans.size()), g_rtViewSector);
	for (auto& pl : rt_portalPlans)
	{
		const int pnum = int(pl.pd - allPortals.Data());
		if (pl.allow.empty()) { Printf("  portal %d: not seen from this side\n", pnum); continue; }
		FString list;
		for (unsigned i = 0; i < pl.allow.size(); i++) if (pl.allow[i]) list.AppendFormat(" %u", i);
		Printf("  portal %d: delta (%.0f %.0f %.0f), %u nearby sectors, %u left out (overlap rooms seen as they are), %u real ones hidden; drawn:%s\n",
			pnum, pl.pd->delta.X, pl.pd->delta.Y, pl.pd->delta.Z, pl.candidates, pl.nearClash, pl.hidden, list.GetChars());
	}
}

void HWDrawInfo::RenderPortal(HWPortal *p, FRenderState &state, bool usestencil)
{
	auto gp = static_cast<HWPortal *>(p);

	if (RT_IsActive())
	{
		// The path tracer does not render portals as recursive views:
		//  - sky: the openings mark where the sky is seen, the dome becomes the sky
		//  - mirror: the opening is an RTGL1 mirror
		//  - anything else: not drawn (yet)
		if (gp->IsSky())
		{
			{
				auto rttype = rtstate.push_type(RtPrim::SkyVisibility);
				gp->RT_DrawStencil(state);
			}
			auto new_di = StartDrawInfo(this, Viewpoint, &VPUniforms);
			new_di->visibility = visibility;
			new_di->rellight = rellight;
			new_di->mCurrentPortal = gp;
			state.SetLightIndex(-1);
			{
				auto rttype = rtstate.push_type(RtPrim::Sky);
				gp->DrawContents(new_di, state);
			}
			new_di->EndDrawInfo();
			state.SetVertexBuffer(screen->mVertexData);
			screen->mViewpoints->Bind(state, vpIndex);
		}
		else if (gp->GetType() == PORTAL_WALL_MIRROR)
		{
			auto rttype = rtstate.push_type(RtPrim::Mirror);
			state.EnableTexture(false);
			gp->RT_DrawStencil(state);
			state.EnableTexture(true);
		}
		else if ((gp->GetType() == PORTAL_SECTOR_FLOOR || gp->GetType() == PORTAL_SECTOR_CEILING) && !gp->rtOffscreen && g_rtCopyUsed < RT_MAX_COPIES && rt_portal_views)
		{
			// a room over a room (Blood): the place elsewhere on the map that the opening
			// shows, moved to where it is seen, with its own ids (and its sectors' lights
			// moved too: g_rtCopySector). Only openings the view sees: one found off screen
			// would put a copy where nothing is meant to show one.
			auto sp = static_cast<HWSectorStackPortal*>(gp);
			const PortalDesc* pd = sp->GetPortalDesc();
			const unsigned n = sector.Size();
			std::vector<char> allow;
			const RtPortalPlan* plan = nullptr;
			if (mCurrentPortal == nullptr)
				for (auto& pl : rt_portalPlans) if (pl.pd == pd) plan = &pl;
			if (plan) allow = plan->allow;
			else
			{
				// seen inside another view (or not planned): the part that overlaps nothing
				// that is drawn
				const double* outer = rtstate.get_copyoffset();
				const DVector3 shift(outer[0] - pd->delta.X, outer[1] - pd->delta.Y, outer[2] - pd->delta.Z);
				const DVector3 cam(g_rtViewPos[0] - outer[0], g_rtViewPos[1] - outer[1], g_rtViewPos[2] - outer[2]);
				if (pd && RT_OpeningFaces(pd, gp->GetType(), cam))
				{
					RT_SectorShapes();
					const std::vector<char> cand = RT_PortalCandidates(pd);
					allow.assign(n, 0);
					std::vector<int> clashes;
					for (unsigned c = 0; c < n; c++)
					{
						if (!cand[c]) continue;
						RT_CopyClashes(c, shift, clashes);
						bool drawn = false;
						for (int r : clashes) drawn |= !RT_SectorHidden(r);
						allow[c] = !drawn;
					}
				}
			}
			bool any = false;
			for (char a : allow) any |= a != 0;
			if (!any) return;

			const int copy = ++g_rtCopyUsed;
			const double ofs[3] = { -pd->delta.X, -pd->delta.Y, -pd->delta.Z };
			for (int i = 0; i < 3; i++) g_rtCopyOffset[copy - 1][i] = ofs[i] + rtstate.get_copyoffset()[i];
			g_rtCopySkipWater[copy - 1] = false;
			// its sectors, for their lights
			if (g_rtCopySector.size() != n) g_rtCopySector.assign(n, 0);
			for (unsigned i = 0; i < n; i++)
				if (allow[i] && g_rtCopySector[i] == 0) g_rtCopySector[i] = uint8_t(copy);

			std::vector<char> outerFilter = std::move(g_rtPortalFilter);
			g_rtPortalFilter = std::move(allow);

			auto new_di = StartDrawInfo(this, Viewpoint, &VPUniforms);
			new_di->visibility = visibility;
			new_di->rellight = rellight;
			new_di->mCurrentPortal = gp;
			state.SetLightIndex(-1);
			{
				auto rttype = rtstate.push_type(RtPrim::CopyMove);
				auto rtcg = rtstate.push_copygroup(copy, ofs, true);
				gp->DrawContents(new_di, state);
			}
			g_rtPortalFilter = std::move(outerFilter);
			new_di->EndDrawInfo();
			state.SetVertexBuffer(screen->mVertexData);
			screen->mViewpoints->Bind(state, vpIndex);
		}
		return;
	}

	gp->SetupStencil(this, state, usestencil);
	auto new_di = StartDrawInfo(this, Viewpoint, &VPUniforms);
	new_di->visibility = visibility;
	new_di->rellight = rellight;
	new_di->mCurrentPortal = gp;
	state.SetLightIndex(-1);
	gp->DrawContents(new_di, state);
	new_di->EndDrawInfo();
	state.SetVertexBuffer(screen->mVertexData);
	screen->mViewpoints->Bind(state, vpIndex);
	gp->RemoveStencil(this, state, usestencil);
}

//-----------------------------------------------------------------------------
//
// Draws player sprites and color blend
//
//-----------------------------------------------------------------------------


void HWDrawInfo::EndDrawScene(FRenderState &state)
{
	state.EnableFog(false);

#if 0
	// [BB] HUD models need to be rendered here. 
	const bool renderHUDModel = IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player);
	if (renderHUDModel)
	{
		// [BB] The HUD model should be drawn over everything else already drawn.
		state.Clear(CT_Depth);
		DrawPlayerSprites(true, state);
	}
#endif

	state.EnableStencil(false);
	state.SetViewport(screen->mScreenViewport.left, screen->mScreenViewport.top, screen->mScreenViewport.width, screen->mScreenViewport.height);

	// Restore standard rendering state
	state.SetRenderStyle(STYLE_Translucent);
	state.ResetColor();
	state.EnableTexture(true);
	state.SetScissor(0, 0, -1, -1);
}


//-----------------------------------------------------------------------------
//
// sets 3D viewport and initial state
//
//-----------------------------------------------------------------------------

void HWDrawInfo::Set3DViewport(FRenderState &state)
{
	// Always clear all buffers with scissor test disabled.
	// This is faster on newer hardware because it allows the GPU to skip
	// reading from slower memory where the full buffers are stored.
	state.SetScissor(0, 0, -1, -1);
	state.Clear(CT_Color | CT_Depth | CT_Stencil);

	const auto &bounds = screen->mSceneViewport;
	state.SetViewport(bounds.left, bounds.top, bounds.width, bounds.height);
	state.SetScissor(bounds.left, bounds.top, bounds.width, bounds.height);
	state.EnableMultisampling(true);
	state.EnableDepthTest(true);
	state.EnableStencil(true);
	state.SetStencil(0, SOP_Keep, SF_AllOn);
}

//-----------------------------------------------------------------------------
//
// gl_drawscene - this function renders the scene from the current
// viewpoint, including mirrors and skyboxes and other portals
// It is assumed that the HWPortal::EndFrame returns with the 
// stencil, z-buffer and the projection matrix intact!
//
//-----------------------------------------------------------------------------

void HWDrawInfo::DrawScene(int drawmode, bool portal)
{
	static int recursion = 0;
	static int ssao_portals_available = 0;

	bool applySSAO = false;
	if (drawmode == DM_MAINVIEW)
	{
		ssao_portals_available = gl_ssao_portals;
		applySSAO = true;
	}
	else if (drawmode == DM_OFFSCREEN)
	{
		ssao_portals_available = 0;
	}
	else if (drawmode == DM_PORTAL && ssao_portals_available > 0)
	{
		applySSAO = (mCurrentPortal->AllowSSAO()/* || Level->flags3&LEVEL3_SKYBOXAO*/);
		ssao_portals_available--;
	}

	CreateScene(portal);
	auto& RenderState = *screen->RenderState();

	RenderState.SetDepthMask(true);

	if (!gl_no_skyclear) portalState.RenderFirstSkyPortal(recursion, this, RenderState);

	RenderScene(RenderState);

	if (applySSAO && RenderState.GetPassType() == GBUFFER_PASS)
	{
		screen->AmbientOccludeScene(VPUniforms.mProjectionMatrix.get()[5]);
		screen->mViewpoints->Bind(RenderState, vpIndex);
	}

	// Handle all portals after rendering the opaque objects but before
	// doing all translucent stuff
	recursion++;
	portalState.EndFrame(this, RenderState);
	recursion--;
	RenderTranslucent(RenderState);
}


//-----------------------------------------------------------------------------
//
// R_RenderView - renders one view - either the screen or a camera texture
//
//-----------------------------------------------------------------------------

void HWDrawInfo::ProcessScene(bool toscreen)
{
	portalState.BeginScene();
	DrawScene(toscreen ? DM_MAINVIEW : DM_OFFSCREEN, false);
	if (toscreen && isBlood())
	{
		gotsector = mDrawer.GotSector(); // Blood needs this to implement some lighting effect hacks. Needs to be refactored to use better info.
	}
}
