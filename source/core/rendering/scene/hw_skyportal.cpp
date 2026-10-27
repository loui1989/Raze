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
#include "bitmap.h"
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
	// its light (the dome's axes, y up): towards its brightest part, its colour
	bool  lightOk = false;
	float lightDir[3] = { 0, 1, 0 }, lightColor[3] = { 1, 1, 1 }, lightIrr = 0, lightDiam = 2;
};

// a sky's light, from its picture above the horizon as the dome shows it: where its
// brightest part is (a moon; a glowing stretch of clouds), how bright, what colour
void RTSkyLightOf(RTSkyDome& d, FGameTexture* tex, double th0, double base, const std::function<double(double)>& tOf, double thTop)
{
	constexpr double pi = 3.14159265358979;
	FBitmap bm = tex->GetTexture()->GetBgraBitmap(nullptr);
	const int W = bm.GetWidth(), H = bm.GetHeight();
	const uint8_t* px = bm.GetPixels();
	if (!px || W <= 0 || H <= 0) return;
	// the elevation of each row of the picture (its rows go up the sky as t falls)
	constexpr int N = 400;
	double rowAt[N + 1];
	for (int i = 0; i <= N; i++) rowAt[i] = (tOf(thTop * i / N) - base) * H;
	auto thOfRow = [&](double row) -> double
	{
		if (row >= rowAt[0]) return -1;	// under the horizon
		for (int i = 0; i < N; i++)
			if (row <= rowAt[i] && row >= rowAt[i + 1])
			{
				const double f = rowAt[i] - rowAt[i + 1] > 1e-9 ? (rowAt[i] - row) / (rowAt[i] - rowAt[i + 1]) : 0;
				return thTop * (i + f) / N;
			}
		return thTop;
	};
	double linLut[256];
	for (int i = 0; i < 256; i++) linLut[i] = pow(i / 255., 2.2);
	auto lin = [&](uint8_t c) { return linLut[c]; };
	// the sky in 10 degree patches: how bright each is on average, its colour, where its
	// light comes from within it
	constexpr int AZ = 36, EL = 9;
	struct Patch { double lum = 0, area = 0, rgb[3] = {}, dir[3] = {}; };
	Patch patch[EL][AZ];
	double avg[3] = {}, area = 0;
	for (int y = 0; y < H; y++)
	{
		const double th = thOfRow(y + 0.5);
		if (th < 0) continue;
		const double dth = fabs(thOfRow(y + 1.) - thOfRow(double(y))) + 1e-9;
		const double dOmega = cos(th) * dth * 2 * pi / W;
		const int e = min(EL - 1, int(th * 180 / pi / 10));
		for (int x = 0; x < W; x++)
		{
			const uint8_t* c = &px[(size_t(y) * W + x) * 4];	// BGRA
			if (c[3] < 128) continue;
			const double r = lin(c[2]), g = lin(c[1]), b = lin(c[0]);
			const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
			const double phi = (0.5 - (x + 0.5) / W) * 2 * pi;
			const int a = (int(floor(phi * 180 / pi / 10)) % AZ + AZ) % AZ;
			auto& pt = patch[e][a];
			pt.lum += lum * dOmega; pt.area += dOmega;
			pt.rgb[0] += r * dOmega; pt.rgb[1] += g * dOmega; pt.rgb[2] += b * dOmega;
			pt.dir[0] += lum * dOmega * cos(th) * cos(phi); pt.dir[1] += lum * dOmega * sin(th); pt.dir[2] += lum * dOmega * cos(th) * sin(phi);
			avg[0] += r * dOmega; avg[1] += g * dOmega; avg[2] += b * dOmega; area += dOmega;
		}
	}
	if (area <= 0) return;
	for (auto& v : avg) v /= area;
	const double avgLum = 0.2126 * avg[0] + 0.7152 * avg[1] + 0.0722 * avg[2];
	if (avgLum <= 1e-6) return;
	// the brightest stretch of sky (a patch with its neighbours: some 30 degrees across)
	double best = -1;
	int be = 0, ba = 0;
	for (int e = 0; e < EL; e++)
		for (int a = 0; a < AZ; a++)
		{
			double l = 0, o = 0;
			for (int de = -1; de <= 1; de++)
				for (int da = -1; da <= 1; da++)
				{
					const int ee = e + de;
					if (ee < 0 || ee >= EL) continue;
					const auto& pt = patch[ee][(a + da + AZ) % AZ];
					l += pt.lum; o += pt.area;
				}
			if (o > 0 && l / o > best) { best = l / o; be = e; ba = a; }
		}
	double dir[3] = {}, rgb[3] = {}, o = 0;
	for (int de = -1; de <= 1; de++)
		for (int da = -1; da <= 1; da++)
		{
			const int ee = be + de;
			if (ee < 0 || ee >= EL) continue;
			const auto& pt = patch[ee][(ba + da + AZ) % AZ];
			for (int i = 0; i < 3; i++) { dir[i] += pt.dir[i]; rgb[i] += pt.rgb[i]; }
			o += pt.area;
		}
	const double len = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (len <= 0 || o <= 0) return;
	// up the sky enough to light the ground and cast shadows of a sensible length (a
	// glow on the horizon lit rooms sideways through their windows), not straight down
	const double az = atan2(dir[2], dir[0]);
	const double el = clamp(atan2(dir[1], sqrt(dir[0] * dir[0] + dir[2] * dir[2])), 30. * pi / 180., 60. * pi / 180.);
	d.lightDir[0] = float(cos(el) * cos(az)); d.lightDir[1] = float(sin(el)); d.lightDir[2] = float(cos(el) * sin(az));
	const double mx = max({ rgb[0], rgb[1], rgb[2] });
	if (mx <= 0) return;
	for (int i = 0; i < 3; i++) d.lightColor[i] = float(0.9 * rgb[i] / mx + 0.1);
	// as bright as the sky's light on the ground (pi times its average radiance), more
	// the more its bright stretch stands out; softer shadows from a wide, even glow
	const double ratio = clamp(best / avgLum, 1., 4.);
	d.lightIrr  = float(pi * avgLum * sqrt(ratio));
	d.lightDiam = float(clamp(12. / ratio, 2., 6.));
	d.lightOk   = true;
}

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
	RTSkyLightOf(d, tex, th0, floor(th0 - 1e-4), [&](double th) { return th0 - (th0 - tTopUp) * min(1., hUp(th)); }, thTop);
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
		const RTSkyDome* rtd = RT_IsActive() ? &RTBuildDome(tex, texh, repeat_fac, -40.f + texskyoffset * skyoffsetfactor) : nullptr;
		if (rtd && rtd->lightOk)
		{
			// its light: in the dome's axes (the backend turns it into the world's with the
			// dome's own transform -- its rotation -- when it draws the sky)
			for (int i = 0; i < 3; i++) g_rtSkyLight.local[i] = rtd->lightDir[i];
			for (int i = 0; i < 3; i++) g_rtSkyLight.color[i] = rtd->lightColor[i];
			g_rtSkyLight.irradiance = rtd->lightIrr;
			g_rtSkyLight.diameter = rtd->lightDiam;
			g_rtSkyLight.pending = true;
		}
		if (rtd && rt_sky_dome)
		{
			const auto& d = *rtd;
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
