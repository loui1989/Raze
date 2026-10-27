// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2003-2016 Christoph Oelckers
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

#include "filesystem.h"
#include "hw_skydome.h"
#include "hw_portal.h"
#include "rt_state.h"
#include "hw_renderstate.h"
#include "skyboxtexture.h"
#include "v_video.h"
#include "hwrenderer/data/buffers.h"
#include <map>
#include <functional>
#include <tuple>

CVAR(Float, skyoffsettest, 0.f, 0)
CVARD(Bool, rt_sky_dome, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "RT: a Build sky's picture reaches up to the zenith (off: the game's dome, its top a flat patch of colour)")

std::pair<PalEntry, PalEntry>& R_GetSkyCapColor(FGameTexture* tex); // hw_skydome.cpp

//-----------------------------------------------------------------------------
//
// Ray tracing: a Build sky's dome for the path tracer. The game's is a cylinder whose
// picture ends some 58 degrees up; above it is a flat cap in the picture's average
// colour -- seen from below, a big smooth blot in the middle of the sky. Here the
// picture is as the game's at the horizon and then is drawn out up to near the
// zenith, without passing the top of the picture (that would bring its bottom -- the
// mountains, a city -- into the sky: a seam); only a small patch at the very top
// (where the picture's columns meet) fades into the cap colour.
//
//-----------------------------------------------------------------------------

namespace
{
struct RTSkyDome
{
	IVertexBuffer* vb = nullptr;
	int capUp = 0, capDn = 0, capN = 0;		// the two cap fans
	int stripUp = 0, stripDn = 0, strips = 0, stripN = 0;	// the picture's rows, each a strip
};

std::map<std::tuple<const void*, float, float, float>, RTSkyDome> s_rtDomes;

const RTSkyDome& RTBuildDome(FGameTexture* tex, float texh, float repeat_fac, float T)
{
	auto key = std::make_tuple((const void*)tex, texh, repeat_fac, T);
	auto it = s_rtDomes.find(key);
	if (it != s_rtDomes.end()) return it->second;

	constexpr int    cols = 128, rows = 24;
	constexpr double R = 10000., pi = 3.14159265358979;
	constexpr double thTop  = 85. * pi / 180.;	// the picture's top row reached here...
	constexpr double fadeLo = 72. * pi / 180., fadeHi = 86. * pi / 180.;	// ...and faded into the cap colour here
	constexpr double thMax  = 86. * pi / 180., capFrom = 66. * pi / 180.;
	const double S = texh / 400.;

	// the game's mapping (its cylinder, 10000 away): the picture's coordinate (in
	// repeats, the texture matrix's scale applied) at elevation th
	auto tGame = [&](double th) { return repeat_fac * (0.5 - (R * tan(th) - T) / (32000. * S)); };
	const double th0 = tGame(0);
	const double k   = repeat_fac * R / (32000. * S);	// its slope at the horizon, per radian
	const double inset = 0.004;	// (inside the picture: filtering does not reach the copy above)

	// a curve from 0 at the horizon to 1 at thTop, starting as the game's does
	auto makeCurve = [&](double range) -> std::function<double(double)>
	{
		const double m = range > 1e-4 ? k / range * thTop : 1.;
		if (m >= 0.999)
		{
			// the game's picture runs out fast (a short sky: Duke's): eases out
			const double p = max(m, 1.);
			return [=](double th) { return 1. - pow(max(0., 1. - th / thTop), p); };
		}
		// it runs out slowly (a tall sky: Blood's): speeds up, as the game's tangent
		// does -- x / tan(x) = m
		double lo = 1e-4, hi = pi / 2 - 1e-4;
		for (int i = 0; i < 60; i++)
		{
			const double mid = (lo + hi) * 0.5;
			if (mid / tan(mid) > m) lo = mid; else hi = mid;
		}
		const double a = (lo + hi) * 0.5 / thTop;
		const double norm = tan(a * thTop);
		return [=](double th) { return tan(a * min(th, thTop)) / norm; };
	};
	const double tTopUp = floor(th0 - 1e-4) + inset;	// the top of the copy the horizon is in
	const double tTopDn = ceil(th0 + 1e-4) - inset;		// and its bottom
	auto hUp = makeCurve(th0 - tTopUp);
	auto hDn = makeCurve(tTopDn - th0);

	TArray<FSkyVertex> v;
	auto put = [&](double th, double phiFrac, double t, int alpha)
	{
		const double phi = phiFrac * 2 * pi;
		FSkyVertex sv;
		sv.SetXYZ(float(R * cos(th) * cos(phi)), float(R * sin(th)), float(R * cos(th) * sin(phi)),
			float(0.5 - phiFrac), float(t), PalEntry(uint8_t(alpha), 255, 255, 255));
		v.Push(sv);
	};
	RTSkyDome d;
	// the caps: a fan from the pole
	d.capN = cols + 2;
	for (int up = 1; up >= 0; up--)
	{
		(up ? d.capUp : d.capDn) = v.Size();
		const double sg = up ? 1 : -1;
		put(sg * pi / 2, 0, 0, 255);
		for (int c = 0; c <= cols; c++) put(sg * capFrom, double(c) / cols, 0, 255);
	}
	// the picture's rows, higher row first
	d.strips = rows;
	d.stripN = (cols + 1) * 2;
	for (int up = 1; up >= 0; up--)
	{
		(up ? d.stripUp : d.stripDn) = v.Size();
		for (int r = 0; r < rows; r++)
		{
			for (int c = 0; c <= cols; c++)
			{
				for (int e = 1; e >= 0; e--)
				{
					const double th = thMax * double(r + e) / rows;
					const double t = up ? th0 - (th0 - tTopUp) * min(1., hUp(th)) : th0 + (tTopDn - th0) * min(1., hDn(th));
					const int alpha = int(255 * clamp((fadeHi - th) / (fadeHi - fadeLo), 0., 1.) + 0.5);
					put(up ? th : -th, double(c) / cols, t, alpha);
				}
			}
		}
	}
	static const FVertexBufferAttribute format[] = {
		{ 0, VATTR_VERTEX, VFmt_Float3, (int)myoffsetof(FSkyVertex, x) },
		{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FSkyVertex, u) },
		{ 0, VATTR_COLOR, VFmt_Byte4, (int)myoffsetof(FSkyVertex, color) },
		{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FSkyVertex, lu) },
	};
	d.vb = screen->CreateVertexBuffer();
	d.vb->SetFormat(1, 4, sizeof(FSkyVertex), format);
	d.vb->SetData(v.Size() * sizeof(FSkyVertex), &v[0], BufferUsageType::Static);
	return s_rtDomes.emplace(key, d).first->second;
}
}

void RT_FreeSkyDomes()
{
	for (auto& [k, d] : s_rtDomes) delete d.vb;
	s_rtDomes.clear();
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------
void HWSkyPortal::DrawContents(HWDrawInfo *di, FRenderState &state)
{
	// one id per sky texture (the portal objects are made anew each frame)
	auto rtid = rtstate.push_uniqueid(origin->texture, RtIdKind::Sky);

	int indexed = hw_int_useindexedcolortextures;
	hw_int_useindexedcolortextures = false; // this code does not work with indexed textures.

	state.SetNoSoftLightLevel();

	state.ResetColor();
	state.EnableFog(false);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.SetRenderStyle(STYLE_Translucent);
	bool oldClamp = state.SetDepthClamp(true);
	auto color = shadeToLight(origin->shade);

	di->SetupView(state, 0, 0, 0, !!(mState->MirrorFlag & 1), !!(mState->PlaneMirrorFlag & 1));

	state.SetVertexBuffer(vertexBuffer);
	state.SetTextureMode(TM_OPAQUE);
	auto skybox = origin->texture ? dynamic_cast<FSkyBox*>(origin->texture->GetTexture()) : nullptr;
	if (skybox)
	{
		vertexBuffer->RenderBox(state, skybox, origin->x_offset, false, /*di->Level->info->pixelstretch*/1, { 0, 0, 1 }, { 0, 0, 1 }, color);
	}
	else if (!origin->cloudy)
	{
		auto tex = origin->texture;
		float texh = tex->GetDisplayHeight();
		auto& modelMatrix = state.mModelMatrix;
		auto& textureMatrix = state.mTextureMatrix;
		auto texskyoffset = tex->GetSkyOffset() + skyoffsettest;
		if (!(g_gameType & GAMEFLAG_PSEXHUMED)) texskyoffset += origin->y_offset;

		float repeat_fac = 1;
		if (texh <= 256)
		{
			repeat_fac = 336.f / texh;
			texh = 336;
		}
		if (RT_IsActive() && rt_sky_dome)
		{
			const auto& d = RTBuildDome(tex, texh, repeat_fac, -40.f + texskyoffset * skyoffsetfactor);
			state.SetVertexBuffer(d.vb, 0, 0);
			state.SetIndexBuffer(nullptr);
			modelMatrix.loadIdentity();
			modelMatrix.rotate(-180.0f + origin->x_offset, 0.f, 1.f, 0.f);
			state.EnableModelMatrix(true);
			// the caps (drawn first: the picture fades into them)
			auto col = R_GetSkyCapColor(tex);
			const PalEntry pc = color;	// (as the game's dome takes it)
			for (auto* c : { &col.first, &col.second })
			{
				c->r = c->r * pc.r / 255; c->g = c->g * pc.g / 255; c->b = c->b * pc.b / 255;
			}
			state.SetMaterial(tex, UF_Texture, 0, CLAMP_NONE, 0, -1);
			state.EnableTexture(false);
			state.SetObjectColor(col.first);
			state.Draw(DT_TriangleFan, d.capUp, d.capN);
			state.SetObjectColor(col.second);
			state.Draw(DT_TriangleFan, d.capDn, d.capN);
			state.EnableTexture(true);
			state.SetObjectColor(0xffffffff);
			for (int r = 0; r < d.strips; r++)
			{
				state.Draw(DT_TriangleStrip, d.stripUp + r * d.stripN, d.stripN);
				state.Draw(DT_TriangleStrip, d.stripDn + r * d.stripN, d.stripN);
			}
			state.EnableModelMatrix(false);
			state.SetVertexBuffer(vertexBuffer);
			goto skydone;
		}
		modelMatrix.loadIdentity();
		modelMatrix.rotate(-180.0f + origin->x_offset, 0.f, 1.f, 0.f);
		modelMatrix.translate(0.f, -40 + texskyoffset * skyoffsetfactor, 0.f);
		modelMatrix.scale(1.f, texh / 400.f, 1.f);
		textureMatrix.loadIdentity();
		state.EnableTextureMatrix(true);
		textureMatrix.scale(1.f, repeat_fac, 1.f);
		vertexBuffer->DoRenderDome(state, origin->texture, FSkyVertexBuffer::SKYMODE_MAINLAYER, true, color);
		state.EnableTextureMatrix(false);
	}
	else
	{
		vertexBuffer->RenderDome(state, origin->texture, -origin->x_offset, origin->y_offset, false, FSkyVertexBuffer::SKYMODE_MAINLAYER, true, 0, 0, color);
	}
skydone:
	state.SetTextureMode(TM_NORMAL);
	if (origin->fadecolor & 0xffffff)
	{
		PalEntry FadeColor = origin->fadecolor;
		state.EnableTexture(false);
		state.SetObjectColor(FadeColor);
		state.Draw(DT_Triangles, 0, 12);
		state.EnableTexture(true);
		state.SetObjectColor(0xffffffff);
	}

	//di->lightmode = oldlightmode;
	state.SetDepthClamp(oldClamp);
	hw_int_useindexedcolortextures = indexed;
 }

const char *HWSkyPortal::GetName() { return "Sky"; }
