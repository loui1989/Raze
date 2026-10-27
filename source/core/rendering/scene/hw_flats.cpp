// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2000-2016 Christoph Oelckers
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
** gl_flat.cpp
** Flat processing
**
*/

#include "matrix.h"
#include "hw_dynlightdata.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "hw_material.h"
#include "hw_drawinfo.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_drawstructs.h"
#include "hw_renderstate.h"
#include "sectorgeometry.h"
#include "hw_sections.h"
#include "texturemanager.h"
#include "buildtiles.h"
#include "rt_state.h"
#include "texinfo.h"
#include "gamecontrol.h"
#include "tiletexture.h"
#include "palettecontainer.h"

// ray tracing: Blood's liquids, by the game's own surface type of the tile (what its
// footsteps sound like): water, lava, goo -- and water or goo that is red is blood
static RtPrim RT_BloodLiquidOf(FGameTexture* tex)
{
	static TMap<int, RtPrim> cache;
	const int idx = tex->GetID().GetIndex();
	if (auto c = cache.CheckKey(idx)) return *c;
	// an animated liquid is judged by its first frame, so all its frames are the same
	// liquid (frame by frame, a muddy pool flipped between blood and water)
	FTextureID base = tex->GetID();
	{
		const int t = legacyTileNum(base);
		for (int k = 1; t >= 0 && k <= 64; k++)
		{
			const auto b = tileGetTextureID(t - k);
			if (t - k >= 0 && b.isValid())
			{
				const auto& pa = GetExtInfo(b).picanm;
				if (pa.num >= k && (pa.type() == PICANM_ANIMTYPE_FWD || pa.type() == PICANM_ANIMTYPE_OSC)) { base = b; break; }
			}
			const auto f = tileGetTextureID(t + k);
			if (f.isValid())
			{
				const auto& pa = GetExtInfo(f).picanm;
				if (pa.num >= k && pa.type() == PICANM_ANIMTYPE_BACK) { base = f; break; }
			}
		}
	}
	if (base != tex->GetID())
	{
		const RtPrim r = RT_BloodLiquidOf(TexMan.GetGameTexture(base));
		cache.Insert(idx, r);
		return r;
	}
	RtPrim r = RtPrim::Identity;
	const int surf = GetExtInfo(tex->GetID()).surftype;
	enum { kSurfWater = 5, kSurfGoo = 13, kSurfLava = 14 };
	if (surf == kSurfLava) r = RtPrim::Lava;
	else if (surf == kSurfWater || surf == kSurfGoo)
	{
		// its colour: red is blood, green goo is slime, else water
		double sum[3] = {};
		int n = 0;
		if (const uint8_t* px = GetRawPixels(tex->GetID()))
		{
			const int count = int(tex->GetTexelWidth() * tex->GetTexelHeight());
			for (int i = 0; i < count; i += 3, n++)
			{
				const PalEntry c = GPalette.BaseColors[px[i]];
				sum[0] += c.r; sum[1] += c.g; sum[2] += c.b;
			}
		}
		const double rr = n ? sum[0] / n : 0, gg = n ? sum[1] / n : 0, bb = n ? sum[2] / n : 0;
		// red: blood; green: slime; anything else (blue, grey, the brown of muddy
		// water) is water
		if (rr > 2.0 * gg && rr > 2.0 * bb) r = RtPrim::BloodPool;
		else if (gg > 1.2 * rr && gg > 1.2 * bb) r = RtPrim::Slime;
		else r = RtPrim::Water;
	}
	cache.Insert(idx, r);
	return r;
}

// ray tracing: the liquids, by their (animated) floor tiles -- the families of
// Duke-RT's FLUIDOVR: water 336, slime 200, lava 1082
static RtPrim RT_LiquidOf(FGameTexture* tex)
{
	if (!RT_IsActive() || !tex) return RtPrim::Identity;
	if (isBlood()) return RT_BloodLiquidOf(tex);
	if (!isDuke()) return RtPrim::Identity;
	const int t = legacyTileNum(tex->GetID());
	if (t >= 336 && t <= 338) return RtPrim::Water;
	if (t >= 200 && t <= 202) return RtPrim::Slime;
	if (t >= 1082 && t <= 1087) return RtPrim::Lava;
	return RtPrim::Identity;
}

#ifdef _DEBUG
CVAR(Int, gl_breaksec, -1, 0)
#endif

//==========================================================================
//
//
//
//==========================================================================
#if 0
void HWFlat::SetupLights(HWDrawInfo *di, FLightNode * node, FDynLightData &lightdata, int portalgroup)
{
	Plane p;

	lightdata.Clear();
	if (renderstyle == STYLE_Add && !di->Level->lightadditivesurfaces)
	{
		dynlightindex = -1;
		return;	// no lights on additively blended surfaces.
	}
	while (node)
	{
		FDynamicLight * light = node->lightsource;

		if (!light->IsActive())
		{
			node = node->nextLight;
			continue;
		}
		iter_dlightf++;

		// we must do the side check here because gl_GetLight needs the correct plane orientation
		// which we don't have for Legacy-style 3D-floors
		double planeh = plane.plane.ZatPoint(light->Pos);
		if ((planeh<light->Z() && ceiling) || (planeh>light->Z() && !ceiling))
		{
			node = node->nextLight;
			continue;
		}

		p.Set(plane.plane.Normal(), plane.plane.fD());
		draw_dlightf += GetLight(lightdata, portalgroup, p, light, false);
		node = node->nextLight;
	}

	dynlightindex = screen->mLights->UploadLights(lightdata);
}
#endif

//==========================================================================
//
//
//
//==========================================================================

void HWFlat::MakeVertices(HWDrawInfo* di)
{
	if (vertcount > 0) return;
	bool canvas = texture->isHardwareCanvas();
	if (Sprite == nullptr)
	{
		TArray<int>* pIndices;
		auto mesh = sectionGeometry.get(&sections[section], plane, geoofs, &pIndices);

		auto ret = screen->mVertexData->AllocVertices(pIndices->Size());
		auto vp = ret.first;
		float base = -(plane == 0 ? sec->floorz : sec->ceilingz);
		for (unsigned i = 0; i < pIndices->Size(); i++)
		{
			auto ii = (*pIndices)[i];
			auto& pt = mesh->vertices[ii];
			auto& uv = mesh->texcoords[ii];
			vp->SetVertex(pt.X, base + pt.Z, pt.Y);
			vp->SetTexCoord(uv.X, canvas ? 1.f - uv.Y : uv.Y);
			vp++;
		}
		vertindex = ret.second;
		vertcount = pIndices->Size();
		normal = mesh->normal;
	}
	else
	{
		DVector2 pos[4];
		double ofsz[4];
		auto cstat = Sprite->cstat;
		if (tspriteGetSlope(Sprite)) cstat &= ~CSTAT_SPRITE_YFLIP;	// NBlood doesn't y-flip slope sprites.
		GetFlatSpritePosition(Sprite, Sprite->pos.XY(), pos, ofsz, true);
		Sprite->cstat = cstat;

		auto ret = screen->mVertexData->AllocVertices(6);
		auto vp = ret.first;
		float x = !(Sprite->cstat & CSTAT_SPRITE_XFLIP) ? 0.f : 1.f;
		float y = !(Sprite->cstat & CSTAT_SPRITE_YFLIP) ? 0.f : 1.f;
		if (Sprite->clipdist & TSPR_SLOPESPRITE)
		{
			DVector3 posi = { di->Viewpoint.Pos.X, -di->Viewpoint.Pos.Y, -di->Viewpoint.Pos.Z };

			// Make adjustments for poorly aligned slope sprites on floors or ceilings
			constexpr float ONPLANE_THRESHOLD = 3.f;
			if (spriteGetZOfSlopef(Sprite, posi.XY(), tspriteGetSlope(Sprite)) < posi.Z)
			{
				float maxofs = -FLT_MAX, minofs = FLT_MAX;
				for (int i = 0; i < 4; i++)
				{
					float vz = -getceilzofslopeptr(Sprite->sectp, posi);
					float sz = z - ofsz[i];
					int diff = vz - sz;
					if (diff > maxofs) maxofs = diff;
					if (diff < minofs) minofs = diff;
				}
				if (maxofs > 0 && minofs >= -ONPLANE_THRESHOLD && maxofs <= ONPLANE_THRESHOLD) z -= maxofs;
			}
			else
			{
				float maxofs = -FLT_MAX, minofs = FLT_MAX;
				for (int i = 0; i < 4; i++)
				{
					float vz = -getflorzofslopeptr(Sprite->sectp, posi);
					float sz = z - ofsz[i];
					int diff = vz - sz;
					if (diff > maxofs) maxofs = diff;
					if (diff < minofs) minofs = diff;
				}
				if (minofs < 0 && maxofs <= -ONPLANE_THRESHOLD && minofs >= ONPLANE_THRESHOLD) z -= minofs;
			}
		}
		else
		{
			if (z < di->Viewpoint.Pos.Z) normal = { 0,1,0 };
			else normal = { 0, -1, 0 };
		}


		unsigned svi = di->SlopeSpriteVertices.Reserve(4);
		auto svp = &di->SlopeSpriteVertices[svi];

		auto& vpt = di->Viewpoint;
		depth = (float)((Sprite->pos.X - vpt.Pos.X) * vpt.TanCos + (-Sprite->pos.Y - vpt.Pos.Y) * vpt.TanSin);

		for (unsigned j = 0; j < 4; j++)
		{
			svp->SetVertex(pos[j].X, z - ofsz[j], -pos[j].Y);
			if (!canvas) svp->SetTexCoord(j == 1 || j == 2 ? 1.f - x : x, j == 2 || j == 3 ? 1.f - y : y);
			else svp->SetTexCoord(j == 1 || j == 2 ? 1.f - x : x, j == 2 || j == 3 ? y : 1.f - y);
			svp++;
		}
		if (Sprite->clipdist & TSPR_SLOPESPRITE)
		{
			FVector3 v1 = {
				di->SlopeSpriteVertices[svi + 1].x - di->SlopeSpriteVertices[svi].x,
				di->SlopeSpriteVertices[svi + 1].y - di->SlopeSpriteVertices[svi].y,
				di->SlopeSpriteVertices[svi + 1].z - di->SlopeSpriteVertices[svi].z };
			FVector3 v2 = {
				di->SlopeSpriteVertices[svi + 2].x - di->SlopeSpriteVertices[svi].x,
				di->SlopeSpriteVertices[svi + 2].y - di->SlopeSpriteVertices[svi].y,
				di->SlopeSpriteVertices[svi + 2].z - di->SlopeSpriteVertices[svi].z };

			normal = (v1 ^ v2).Unit();
		}
		for (unsigned i = 0; i < 6; i++)
		{
			const static unsigned indices[] = { 0, 1, 2, 0, 2, 3 };
			*vp++ = di->SlopeSpriteVertices[svi + indices[i]];
		}
		slopeindex = svi;
		slopecount = 4;
		vertindex = ret.second;
		vertcount = 6;
	}
}

//==========================================================================
//
//
//
//==========================================================================
void HWFlat::DrawFlat(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	rendered_flats++;
	if (screen->BuffersArePersistent() && !Sprite)
	{
		MakeVertices(di);
	}

#ifdef _DEBUG
	if (sectindex(sec) == gl_breaksec)
	{
		int a = 0;
	}
#endif

	// ray tracing: one id per SECTION, not per sector. A sector's sections are drawn in
	// the order they come into view, so with one id for all of them their primitive
	// numbers swap as the view turns -- and RTGL1 then pairs each with another part's
	// last-frame position: wrong motion, smeared edges
	if (RT_IsActive() && RT_SkipSector(Sprite && Sprite->sectp ? sectindex(Sprite->sectp) : sec ? sectindex(sec) : -1)) return;
	auto rtid = rtstate.push_uniqueid(Sprite ? (const void*)Sprite->ownerActor : (const void*)&sections[section], Sprite ? RtIdKind::FlatSprite : plane ? RtIdKind::CeilingFlat : RtIdKind::FloorFlat);
	RT_PUSH_TYPE(rtfb, Sprite && Sprite->shade <= -numshades ? RtPrim::Fullbright : RtPrim::Identity);
	RT_PUSH_TYPE(rtdecal, Sprite ? RtPrim::Decal : RtPrim::Identity);
	RT_PUSH_TYPE(rtliquid, Sprite ? RtPrim::Identity : RT_LiquidOf(texture));
	auto rtsec = rtstate.push_sector(Sprite && Sprite->sectp ? sectindex(Sprite->sectp) : sec ? sectindex(sec) : -1);
	RT_PUSH_TYPE(rtcopy, Sprite ? (Sprite->rtcopy ? RtPrim::CopyId : RtPrim::Identity) : rtCopy ? RtPrim::CopyMove : RtPrim::Identity);
	auto rtcg = rtstate.push_copygroup(Sprite ? Sprite->rtcopy : rtCopy);
	RT_PUSH_TYPE(rtplane, Sprite ? RtPrim::Identity : plane ? RtPrim::CeilingPlane : RtPrim::FloorPlane);
	// (a floor or ceiling is seen only from its own sector, as a wall)
	RT_PUSH_TYPE(rtonesided, Sprite ? RtPrim::Identity : RtPrim::OneSided);

	state.SetNormal(normal);
	SetLightAndFog(di, state, fade, palette, shade, visibility, alpha);

	if (translucent)
	{
		if (RenderStyle.BlendOp != STYLEOP_Add)
		{
			state.EnableBrightmap(false);
		}

		state.SetRenderStyle(RenderStyle);
		state.SetTextureMode(RenderStyle);

		if (texture && !checkTranslucentReplacement(texture->GetID(), palette)) state.AlphaFunc(Alpha_GEqual, texture->alphaThreshold);
		else state.AlphaFunc(Alpha_GEqual, 0.f);
	}
	else if (shade > numshades && (GlobalMapFog || (fade & 0xffffff)) && !RT_IsActive())
	{
		state.SetObjectColor(fade | 0xff000000);
		state.EnableTexture(false);
	}


	state.SetMaterial(texture, UF_Texture, 0, Sprite == nullptr? CLAMP_NONE : CLAMP_XY, TRANSLATION(Translation_Remap + curbasepal, palette).index(), -1);

	state.SetLightIndex(dynlightindex);
	state.Draw(DT_Triangles, vertindex, vertcount);
	vertexcount += vertcount;

	// ray tracing: a liquid floor with nothing under it (Blood's shallow pools and
	// puddles: no water link, no room below). The path tracer sees through liquids,
	// down to the sky: a bottom just under the surface, its own floor again, solid
	if (RT_IsActive() && !Sprite && plane == 0 && isBlood() && sec && sec->upperLink == nullptr &&
		sec->portalflags != PORTAL_SECTOR_FLOOR)
	{
		const RtPrim liq = RT_LiquidOf(texture);
		if (liq == RtPrim::Water || liq == RtPrim::Slime || liq == RtPrim::BloodPool)
		{
			auto rtsolid = rtstate.push_without(RtPrim(uint32_t(RtPrim::Water) | uint32_t(RtPrim::Slime) | uint32_t(RtPrim::BloodPool)));
			RT_PUSH_TYPE(rtbottom, RtPrim::CopyMove);
			// just under it (z down): deeper, the rays find the gap between the bottom
			// and the pool's edges, and the sky through it
			static const double down[3] = { 0, 0, 1.5 };
			auto rtbcg = rtstate.push_copygroup(64 + rtstate.get_copygroup(), down, true);
			state.Draw(DT_Triangles, vertindex, vertcount);
		}
	}

	if (translucent) state.SetRenderStyle(LegacyRenderStyles[STYLE_Translucent]);
	state.EnableBrightmap(true);

	state.SetObjectColor(0xffffffff);
	state.EnableTexture(true);
	//state.SetAddColor(0);
	//state.ApplyTextureManipulation(nullptr);
}

//==========================================================================
//
// HWFlat::PutFlat
//
// submit to the renderer
//
//==========================================================================

void HWFlat::PutFlat(HWDrawInfo *di, int whichplane)
{
	vertcount = 0;
	plane = whichplane;
	if (!screen->BuffersArePersistent() || Sprite || di->ingeo)	// should be made static buffer content later (when the logic is working)
	{
#if 0
		if (di->Level->HasDynamicLights && texture != nullptr && !di->isFullbrightScene() && !(hacktype & (SSRF_PLANEHACK | SSRF_FLOODHACK)))
		{
			SetupLights(di, section->lighthead, lightdata, sector->PortalGroup);
		}
#endif
		MakeVertices(di);
	}
	di->AddFlat(this);
}

//==========================================================================
//
// Process a sector's flats for rendering
// This function is only called once per sector.
// Subsequent subsectors are just quickly added to the ss_renderflags array
//
//==========================================================================

void HWFlat::ProcessSector(HWDrawInfo *di, sectortype * frontsector, int section_, int which)
{
#ifdef _DEBUG
	if (sectindex(frontsector) == gl_breaksec)
	{
		int a = 0;
	}
#endif

	dynlightindex = -1;

	const auto &vp = di->Viewpoint;

	float florz, ceilz;
	PlanesAtPoint(frontsector, vp.Pos.X, -vp.Pos.Y, &ceilz, &florz);

	visibility = sectorVisibility(frontsector);
	frontsector->exflags |= SECTOREX_SEEN;
	sec = frontsector;
	section = section_;
	Sprite = nullptr;
	geoofs = di->geoofs;

	//
	//
	//
	// do floors
	//
	//
	//
	// (the path tracer needs the planes that face away from the camera too)
	if ((which & SSRF_RENDERFLOOR) && !(frontsector->floorstat & CSTAT_SECTOR_SKY) && (florz <= vp.Pos.Z || RT_IsActive()))
	{
		// process the original floor first.

		fade = lookups.getFade(frontsector->floorpal);
		shade = frontsector->floorshade;
		palette = frontsector->floorpal;
		stack = frontsector->portalflags == PORTAL_SECTOR_FLOOR || frontsector->portalflags == PORTAL_SECTOR_FLOOR_REFLECT;

		if (stack && (frontsector->floorstat & CSTAT_SECTOR_METHOD))
		{
			RenderStyle = GetRenderStyle(0, !!(frontsector->floorstat & CSTAT_SECTOR_TRANS_INVERT));
			alpha = GetAlphaFromBlend((frontsector->floorstat & CSTAT_SECTOR_TRANS_INVERT) ? DAMETH_TRANS2 : DAMETH_TRANS1, 0);
		}
		else
		{
			RenderStyle = STYLE_Translucent;
			alpha = 1.f;
		}

		if (alpha != 0.f)
		{
			auto texid = frontsector->floortexture;
			texture = TexMan.GetGameTexture(texid, true);
			if (texture && texture->isValid())
			{
				//iboindex = frontsector->iboindex[sector_t::floor];
				PutFlat(di, 0);
			}
		}
	}

	//
	//
	//
	// do ceilings
	//
	// 
	//
	if ((which & SSRF_RENDERCEILING) && !(frontsector->ceilingstat & CSTAT_SECTOR_SKY) && (ceilz >= vp.Pos.Z || RT_IsActive()))
	{
		// process the original ceiling first.

		fade = lookups.getFade(frontsector->ceilingpal);
		shade = frontsector->ceilingshade;
		palette = frontsector->ceilingpal;
		stack = frontsector->portalflags == PORTAL_SECTOR_CEILING || frontsector->portalflags == PORTAL_SECTOR_CEILING_REFLECT;

		if (stack && (frontsector->ceilingstat & CSTAT_SECTOR_METHOD))
		{
			RenderStyle = GetRenderStyle(0, !!(frontsector->ceilingstat & CSTAT_SECTOR_TRANS_INVERT));
			alpha = GetAlphaFromBlend((frontsector->ceilingstat & CSTAT_SECTOR_TRANS_INVERT) ? DAMETH_TRANS2 : DAMETH_TRANS1, 0);
		}
		else
		{
			RenderStyle = STYLE_Translucent;
			alpha = 1.f;
		}


		if (alpha != 0.f)
		{
			//iboindex = frontsector->iboindex[sector_t::ceiling];

			auto texid = frontsector->ceilingtexture;
			texture = TexMan.GetGameTexture(texid, true);
			if (texture && texture->isValid())
			{
				//iboindex = frontsector->iboindex[sector_t::floor];
				PutFlat(di,  1);
			}
		}
	}
}

//==========================================================================
//
// Process a floor sprite
//
//==========================================================================

void HWFlat::ProcessFlatSprite(HWDrawInfo* di, tspritetype* sprite, sectortype* sector)
{
	texture = TexMan.GetGameTexture(sprite->spritetexture());
	bool belowfloor = false;
	if (sprite->pos.Z > sprite->sectp->floorz)
	{
		belowfloor = true;
		sprite->pos.Z = sprite->sectp->floorz;
	}
	z = -sprite->pos.Z;
	if (z == di->Viewpoint.Pos.Z) return; // looking right at the edge.
	dynlightindex = -1;

	visibility = sectorVisibility(sector) *(4.f / 5.f); // The factor comes directly from Polymost. What is it with Build and these magic factors?

	// Weird Build logic that really makes no sense.
	if ((sprite->cstat & CSTAT_SPRITE_ONE_SIDE) != 0 && !RT_IsActive())
	{
		DVector2 pos = { di->Viewpoint.Pos.X, -di->Viewpoint.Pos.Y };

		double myz = !(sprite->clipdist & TSPR_SLOPESPRITE) ? z : -spriteGetZOfSlopef(sprite, pos, tspriteGetSlope(sprite));
		if ((di->Viewpoint.Pos.Z < myz) == ((sprite->cstat & CSTAT_SPRITE_YFLIP) == 0))
			return;
	}

	if (texture && texture->isValid())
	{
		this->Sprite = sprite;
		sec = sector;
		shade = sprite->shade;
		palette = sprite->pal;
		fade = lookups.getFade(sector->floorpal);	// fog is per sector.

		SetSpriteTranslucency(sprite, alpha, RenderStyle);
		if (belowfloor) alpha *= 0.33f;

		PutFlat(di, z > di->Viewpoint.Pos.Z);
	}
}
