//-------------------------------------------------------------------------
/*
Copyright (C) 2010-2019 EDuke32 developers and contributors
Copyright (C) 2019 Nuke.YKT

This file is part of NBlood.

NBlood is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
//-------------------------------------------------------------------------
#include "ns.h"	// Must come before everything else!

#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "v_font.h"

#include "blood.h"
#include "choke.h"
#include "zstring.h"
#include "razemenu.h"
#include "gstrings.h"
#include "v_2ddrawer.h"
#include "v_video.h"
#include "v_font.h"
#include "statusbar.h"
#include "automap.h"
#include "gamefuncs.h"
#include "v_draw.h"
#include "precache.h"
#include "render.h"
#include "razefont.h"
#include "rt_state.h"
#include "colormatcher.h"
#include "palettecontainer.h"

EXTERN_CVAR(Float, rt_sprite_light_scale)


BEGIN_BLD_NS

VIEWPOS gViewPos;
int gViewIndex;

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

void viewBackupView(DBloodPlayer* pPlayer)
{
	const auto pActor = pPlayer->GetActor();
	pPlayer->ozView = pPlayer->zView;
	pPlayer->ozWeapon = pPlayer->zWeapon - pPlayer->zView - 12;
	pPlayer->obobHeight = pPlayer->bobHeight;
	pPlayer->obobWidth = pPlayer->bobWidth;
	pPlayer->oswayHeight = pPlayer->swayHeight;
	pPlayer->oswayWidth = pPlayer->swayWidth;
	pActor->backuploc();
	pActor->interpolated = true;
}

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

void viewDrawText(FFont* pFont, const char* pString, int x, int y, int nShade, int nPalette, int position, bool shadow)
{
	if (!pString) return;

	//y += pFont->yoff;

	if (position == 1) x -= pFont->StringWidth(pString) / 2;
	if (position == 2) x -= pFont->StringWidth(pString);


	if (shadow)
	{
		DrawText(twod, pFont, CR_UNTRANSLATED, x + 1, y + 1, pString, DTA_FullscreenScale, FSMode_Fit320x200, DTA_Color, 0xff000000, DTA_Alpha, 0.5, TAG_DONE);
	}
	DrawText(twod, pFont, CR_NATIVEPAL, x, y, pString, DTA_FullscreenScale, FSMode_Fit320x200, DTA_TranslationIndex, TRANSLATION(Translation_Remap, nPalette),
		DTA_Color, shadeToLight(nShade), TAG_DONE);

}

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

void viewDrawAimedPlayerName(DBloodPlayer* pPlayer)
{
	if (!cl_idplayers || (pPlayer->aim.XY().isZero()))
		return;

	int hit = HitScan(pPlayer->GetActor(), pPlayer->zView, pPlayer->aim, CLIPMASK0, 512);
	if (hit == 3)
	{
		auto actor = gHitInfo.actor();
		if (actor && actor->IsPlayerActor())
		{
			int nPlayer = actor->spr.type - kDudePlayer1;
			const char* szName = PlayerName(nPlayer);
			int nPalette = (getPlayer(nPlayer)->teamId & 3) + 11;
			viewDrawText(DigiFont, szName, 160, 125, -128, nPalette, 1, 1);
		}
	}
}

extern DAngle random_angles[16][3];

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

void viewInit(void)
{
	Printf("Initializing status bar\n");

	for (int i = 0; i < 16; i++)
	{
		random_angles[i][0] = RandomAngle();
		random_angles[i][1] = RandomAngle();
		random_angles[i][2] = RandomAngle();
	}
}

int othercameradist = 1280;
int othercameraclock;

//---------------------------------------------------------------------------
//
// 
//
//---------------------------------------------------------------------------

#if 0
void CalcOtherPosition(DBloodActor* actor, int* pX, int* pY, int* pZ, sectortype** vsectnum, int nAng, fixed_t zm, int smoothratio) // currently unused
{
	int vX = MulScale(-Cos(nAng), 1280, 30);
	int vY = MulScale(-Sin(nAng), 1280, 30);
	int vZ = FixedToInt(MulScale(zm, 1280, 3)) - (16 << 8);
	int bakCstat = pSprite->cstat;
	pSprite->cstat &= ~CSTAT_SPRITE_BLOCK_HITSCAN;
	assert(validSectorIndex(*vsectnum));
	FindSector(*pX, *pY, *pZ, vsectnum);
	int nHSector;
	int hX, hY;
	vec3_t pos = { *pX, *pY, *pZ };
	hitscan(&pos, *vsectnum, vX, vY, vZ, &hitdata, CLIPMASK1);
	nHSector = hitdata.sect;
	hX = hitdata.pos.x;
	hY = hitdata.pos.y;
	int dX = hX - *pX;
	int dY = hY - *pY;
	if (abs(vX) + abs(vY) > abs(dX) + abs(dY))
	{
		*vsectnum = nHSector;
		dX -= Sgn(vX) << 6;
		dY -= Sgn(vY) << 6;
		int nDist;
		if (abs(vX) > abs(vY))
		{
			nDist = ClipHigh(DivScale(dX, vX, 16), othercameradist);
		}
		else
		{
			nDist = ClipHigh(DivScale(dY, vY, 16), othercameradist);
		}
		othercameradist = nDist;
	}
	*pX += MulScale(vX, othercameradist, 16);
	*pY += MulScale(vY, othercameradist, 16);
	*pZ += MulScale(vZ, othercameradist, 16);
	int myclock = PlayClock + MulScale(4, smoothratio, 16);
	othercameradist = ClipHigh(othercameradist + ((myclock - othercameraclock) << 10), 65536);
	othercameraclock = myclock;
	assert(validSectorIndex(*vsectnum));
	FindSector(*pX, *pY, *pZ, vsectnum);
	pSprite->cstat = bakCstat;
}
#endif

//---------------------------------------------------------------------------
//
// by NoOne: show warning msgs in game instead of throwing errors (in some cases)
//
//---------------------------------------------------------------------------

void viewSetSystemMessage(const char* pMessage, ...) {
	char buffer[1024]; va_list args; va_start(args, pMessage);
	vsprintf(buffer, pMessage, args);

	Printf(PRINT_HIGH | PRINT_NOTIFY, "%s\n", buffer); // print it also in console
}

void viewSetMessage(const char* pMessage, const char* color, const MESSAGE_PRIORITY priority)
{
	int printlevel = priority <= MESSAGE_PRIORITY_NORMAL ? PRINT_LOW : priority < MESSAGE_PRIORITY_SYSTEM ? PRINT_MEDIUM : PRINT_HIGH;
	Printf(printlevel | PRINT_NOTIFY, "%s%s\n", color? color : TEXTCOLOR_TAN, pMessage);
}

void viewSetErrorMessage(const char* pMessage)
{
	Printf(PRINT_BOLD | PRINT_NOTIFY, "%s\n", pMessage);
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void DoLensEffect(void)
{
	// the lens effect depends on the software renderer and easy availability of the pixel data. 
	// If this ever gets redone for hardware rendering it needs very different handling on the GPU side.
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void UpdateDacs(int nPalette, bool bNoTint)
{
	gLastPal = 0;
	auto& tint = lookups.tables[MAXPALOOKUPS - 1];
	tint.tintFlags = 0;
	switch (nPalette)
	{
	case 0:
	default:
		tint.tintColor.r = 255;
		tint.tintColor.g = 255;
		tint.tintColor.b = 255;
		break;
	case 1:
		tint.tintColor.r = 132;
		tint.tintColor.g = 164;
		tint.tintColor.b = 255;
		break;
	case 2:
		tint.tintColor.r = 255;
		tint.tintColor.g = 126;
		tint.tintColor.b = 105;
		break;
	case 3:
		tint.tintColor.r = 162;
		tint.tintColor.g = 186;
		tint.tintColor.b = 15;
		break;
	case 4:
		tint.tintColor.r = 255;
		tint.tintColor.g = 255;
		tint.tintColor.b = 255;
		break;
	}
	videoSetPalette(nPalette);
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void UpdateBlend(DBloodPlayer* pPlayer)
{
	int nRed = 0;
	int nGreen = 0;
	int nBlue = 0;

	nRed += pPlayer->pickupEffect;
	nGreen += pPlayer->pickupEffect;
	nBlue -= pPlayer->pickupEffect;

	nRed += ClipHigh(pPlayer->painEffect, 85) * 2;
	nGreen -= ClipHigh(pPlayer->painEffect, 85) * 3;
	nBlue -= ClipHigh(pPlayer->painEffect, 85) * 3;

	nRed -= pPlayer->blindEffect;
	nGreen -= pPlayer->blindEffect;
	nBlue -= pPlayer->blindEffect;

	nRed -= pPlayer->chokeEffect >> 6;
	nGreen -= pPlayer->chokeEffect >> 5;
	nBlue -= pPlayer->chokeEffect >> 6;

	nRed = ClipRange(nRed, -255, 255);
	nGreen = ClipRange(nGreen, -255, 255);
	nBlue = ClipRange(nBlue, -255, 255);

	videoTintBlood(nRed, nGreen, nBlue);
}

// int gVisibility;

int deliriumTilt, deliriumPitch;
int deliriumPitchO;
DAngle deliriumTurnO, deliriumTurn;
DAngle gScreenTiltO, gScreenTilt;

int gShowFrameRate = 1;

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void viewUpdateDelirium(DBloodPlayer* pPlayer)
{
	gScreenTiltO = gScreenTilt;
	deliriumTurnO = deliriumTurn;
	deliriumPitchO = deliriumPitch;
	int powerCount;
	if ((powerCount = powerupCheck(pPlayer, kPwUpDeliriumShroom)) != 0)
	{
		int tilt1 = 170, tilt2 = 170, pitch = 20;
		int timer = PlayClock * 2;
		if (powerCount < 512)
		{
			int powerScale = IntToFixed(powerCount) / 512;
			tilt1 = MulScale(tilt1, powerScale, 16);
			tilt2 = MulScale(tilt2, powerScale, 16);
			pitch = MulScale(pitch, powerScale, 16);
		}
		double sin2 = BobVal(2 * timer);
		double sin3 = BobVal(3 * timer);
		double sin4 = BobVal(4 * timer);
		double sin5 = BobVal(5 * timer);
		gScreenTilt = DAngle::fromBuild((sin2 + sin3) * tilt1 * 0.5);
		deliriumTurn = DAngle::fromBuild((sin3 + sin4) * tilt2 * 0.5);
		deliriumPitch = int((sin4 + sin5) * pitch * 0.5);
		return;
	}
	gScreenTilt = gScreenTilt.Normalized180();
	if (gScreenTilt > nullAngle)
	{
		gScreenTilt -= mapangle(8);
		if (gScreenTilt < nullAngle)
			gScreenTilt = nullAngle;
	}
	else if (gScreenTilt < nullAngle)
	{
		gScreenTilt += mapangle(8);
		if (gScreenTilt >= nullAngle)
			gScreenTilt = nullAngle;
	}
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void viewUpdateShake(DBloodPlayer* pPlayer, DVector3& cPos, DRotator& cAngles, double& pshakeX, double& pshakeY)
{
	auto doEffect = [&](const int& effectType)
	{
		if (effectType)
		{
			int nValue = ClipHigh(effectType * 8, 2000);
			cAngles.Pitch -= maphoriz(QRandom2F(nValue * (1. / 256.)));
			cAngles.Yaw += DAngle::fromDeg(QRandom2F(nValue * (360. / 524288.)));
			cPos.X += QRandom2F(nValue * maptoworld) * maptoworld;
			cPos.Y += QRandom2F(nValue * maptoworld) * maptoworld;
			cPos.Z += QRandom2F(nValue) * zmaptoworld;
			pshakeX += QRandom2F(nValue) * (1. / 256.);
			pshakeY += QRandom2F(nValue) * (1. / 256.);
		}
	};
	doEffect(pPlayer->flickerEffect);
	doEffect(pPlayer->quakeEffect);

	cAngles.Pitch -= DAngle::fromDeg((1 - BobVal((pPlayer->tiltEffect << 2) + 512)) * 13.2);
}


int gLastPal = 0;

int32_t g_frameRate;

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

static void DrawMap(DBloodPlayer* pPlayer, const double interpfrac)
{
	int tm = 0;
	if (viewport3d.Left() > 0)
	{
		setViewport(Hud_Stbar);
		tm = 1;
	}
	DrawOverheadMap(pPlayer->GetActor()->interpolatedpos(interpfrac).XY(), pPlayer->getRenderAngles(interpfrac).Yaw, interpfrac);
	if (tm)
		setViewport(hud_size);
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

static void SetupView(DBloodPlayer* pPlayer, DVector3& cPos, DRotator& cAngles, sectortype*& pSector, double& zDelta, double& shakeX, double& shakeY, const double interpfrac)
{
	double bobWidth, bobHeight;

	pSector = pPlayer->GetActor()->sector();
#if 0
	if (numplayers > 1 && pPlayer == gMe && gPrediction && gMe->actor->xspr.health > 0)
	{
		nSectnum = predict.sectnum;
		cX = interpolatedvalue(predictOld.x, predict.x, interpfrac);
		cY = interpolatedvalue(predictOld.y, predict.y, interpfrac);
		cZ = interpolatedvalue(predictOld.viewz, predict.viewz, interpfrac);
		zDelta = interpolatedvalue(predictOld.weaponZ, predict.weaponZ, interpfrac);
		bobWidth = interpolatedvalue(predictOld.bobWidth, predict.bobWidth, interpfrac);
		bobHeight = interpolatedvalue(predictOld.bobHeight, predict.bobHeight, interpfrac);
		shakeX = interpolatedvalue(predictOld.shakeBobX, predict.shakeBobX, interpfrac);
		shakeY = interpolatedvalue(predictOld.shakeBobY, predict.shakeBobY, interpfrac);

		if (!SyncInput())
		{
			cAngles.Yaw = bamang(predict.angle.asbam() + predict.look_ang.asbam());
			cAngles.Pitch = predict.horiz + predict.horizoff;
			cAngles.Roll = predict.cAngles.Roll;
		}
		else
		{
			cAngles.Yaw = interpolatedvalue(predictOld.angle + predictOld.look_ang, predict.angle + predict.look_ang, interpfrac);
			cAngles.Pitch = interpolatedvalue(predictOld.horiz + predictOld.horizoff, predict.horiz + predict.horizoff, interpfrac);
			cAngles.Roll = interpolatedvalue(predictOld.rotscrnang, predict.rotscrnang, interpfrac);
		}
	}
	else
#endif
	{
		cPos = pPlayer->GetActor()->getRenderPos(interpfrac);
		cAngles = pPlayer->getRenderAngles(interpfrac);
		zDelta = interpolatedvalue(pPlayer->ozWeapon, pPlayer->zWeapon - pPlayer->zView - 12, interpfrac);
		bobWidth = interpolatedvalue(pPlayer->obobWidth, pPlayer->bobWidth, interpfrac);
		bobHeight = interpolatedvalue(pPlayer->obobHeight, pPlayer->bobHeight, interpfrac);
		shakeX = interpolatedvalue(pPlayer->oswayWidth, pPlayer->swayWidth, interpfrac);
		shakeY = interpolatedvalue(pPlayer->oswayHeight, pPlayer->swayHeight, interpfrac);
	}

	viewUpdateShake(pPlayer, cPos, cAngles, shakeX, shakeY);

	if (gViewPos == 0)
	{
		if (cl_viewhbob)
		{
			cPos -= cAngles.Yaw.ToVector().Rotated90CW() * bobWidth;
		}
		if (cl_viewvbob)
		{
			cPos.Z += bobHeight;
		}
		cPos.Z -= 10. * (cAngles.Pitch / DAngle90);
	}
	else
	{
		calcChaseCamPos(cPos, pPlayer->GetActor(), &pSector, cAngles, interpfrac, 80.);
	}
	if (pSector != nullptr)
		CheckLink(cPos, &pSector);
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void renderCrystalBall()
{
#if 0
	int tmp = (PlayClock / 240) % (gNetPlayers - 1);
	int i = connecthead;
	while (1)
	{
		if (i == gViewIndex)
			i = connectpoint2[i];
		if (tmp == 0)
			break;
		i = connectpoint2[i];
		tmp--;
	}
	PLAYER* pOther = &gPlayer[i];
	//othercameraclock = PlayClock + MulScale(4, (int)gInterpolate, 16);;
	//renderSetTarget(4079, 128, 128);
	renderSetAspect(65536, 78643);
	int vd8 = pOther->actor->spr.x;
	int vd4 = pOther->actor->spr.y;
	int vd0 = pOther->zView;
	int vcc = pOther->actor->spr.sectnum;
	int v50 = pOther->actor->spr.angle;
	int v54 = 0;
	if (pOther->flickerEffect)
	{
		int nValue = ClipHigh(pOther->flickerEffect * 8, 2000);
		v54 += QRandom2(nValue >> 8);
		v50 += QRandom2(nValue >> 8);
		vd8 += QRandom2(nValue >> 4);
		vd4 += QRandom2(nValue >> 4);
		vd0 += QRandom2(nValue);
	}
	if (pOther->quakeEffect)
	{
		int nValue = ClipHigh(pOther->quakeEffect * 8, 2000);
		v54 += QRandom2(nValue >> 8);
		v50 += QRandom2(nValue >> 8);
		vd8 += QRandom2(nValue >> 4);
		vd4 += QRandom2(nValue >> 4);
		vd0 += QRandom2(nValue);
	}
	CalcOtherPosition(pOther->actor, &vd8, &vd4, &vd0, &vcc, v50, 0, (int)gInterpolate);
	CheckLink(&vd8, &vd4, &vd0, &vcc);
	uint8_t v14 = 0;
	if (IsUnderwaterSector(vcc))
	{
		v14 = 10;
	}
	drawrooms(vd8, vd4, vd0, v50, v54, vcc);
	viewProcessSprites(vd8, vd4, vd0, v50, gInterpolate);
	renderDrawMasks();
#endif
}


//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
//
// ray tracing: water (and goo) the rays see through. Blood's water is two places
// linked by markers: the pool's surface on one side, the room under it somewhere
// else on the map, and the player is moved between the two. The path tracer gets
// a copy of the other place on the far side of the surface (as for Duke's water):
// under water, the world above the surface over the room; above it, the room
// under the water below the surface. A link drawn as a room over a room (the
// mirror tile) needs none: the renderer copies those itself.
//
//---------------------------------------------------------------------------

static void RT_BloodWaterCopy(sectortype* sect, const DVector3& cpos)
{
	g_rtWaterSide.clear();
	if (!sect) return;
	updatesector(cpos.XY(), &sect);
	if (!sect) return;

	auto isWaterLink = [](DCoreActor* a, bool lower)
	{
		if (!a) return false;
		const int t = static_cast<DBloodActor*>(a)->spr.type;
		return lower ? (t == kMarkerLowWater || t == kMarkerLowGoo) : (t == kMarkerUpWater || t == kMarkerUpGoo);
	};
	// a sector's link through its surface (the ceiling under water, the floor above),
	// and the marker at its other end
	auto linkOf = [&](sectortype* s, bool lower, DBloodActor** other) -> DBloodActor*
	{
		auto m = barrier_cast<DBloodActor*>(lower ? s->lowerLink : s->upperLink);
		if (!isWaterLink(m, lower) || s->portalflags == (lower ? PORTAL_SECTOR_CEILING : PORTAL_SECTOR_FLOOR)) return nullptr;
		auto o = static_cast<DBloodActor*>(m->GetOwner());
		if (!o || !o->insector() || !isWaterLink(o, !lower)) return nullptr;
		*other = o;
		return m;
	};

	const unsigned n = sector.Size();
	g_rtWaterSide.assign(n, 0);
	bool any = false;
	for (unsigned i = 0; i < n; i++)
	{
		auto s = &sector[i];
		if (isWaterLink(s->upperLink, false)) g_rtWaterSide[i] = 1, any = true;
		if (IsUnderwaterSector(s) || isWaterLink(s->lowerLink, true)) g_rtWaterSide[i] = 2;
	}
	if (!any) { g_rtWaterSide.clear(); return; }

	DBloodActor* o;
	const bool under = IsUnderwaterSector(sect) || linkOf(sect, true, &o);

	constexpr double reach = 1600; // 50 m
	struct WaterCopy
	{
		DVector3 delta;          // this side + delta = the other side
		sectortype* other;
		double dist;
		TArray<int> seeds;       // the sectors on this side with links
	};
	TArray<WaterCopy> copies;
	for (unsigned i = 0; i < n; i++)
	{
		auto s = &sector[i];
		DBloodActor* far = nullptr;
		auto m = linkOf(s, under, &far);
		if (!m) continue;
		const double d = s == sect ? 0. : (m->spr.pos.XY() - cpos.XY()).Length();
		if (d > reach) continue;
		const double dz = under ? far->sector()->floorz - s->ceilingz : far->sector()->ceilingz - s->floorz;
		const DVector3 delta(far->spr.pos.X - m->spr.pos.X, far->spr.pos.Y - m->spr.pos.Y, dz);

		WaterCopy* c = nullptr;
		for (auto& cc : copies)
			if (fabs(cc.delta.X - delta.X) < 4 && fabs(cc.delta.Y - delta.Y) < 4 && fabs(cc.delta.Z - delta.Z) < 4) { c = &cc; break; }
		if (!c)
		{
			copies.Push({ delta, far->sector(), d, {} });
			c = &copies.Last();
		}
		else if (d < c->dist)
		{
			c->dist = d;
			c->other = far->sector();
		}
		c->seeds.Push(int(i));
	}
	if (copies.Size() == 0) { g_rtWaterSide.clear(); return; }

	// each copy's surfaces: the linked sectors, and the sectors next to them with the
	// same surface (no link of their own), each going to the nearest link's copy
	auto sameSurface = [&](sectortype* a, sectortype* b)
	{
		if (b->portalflags || (under ? b->lowerLink != nullptr : b->upperLink != nullptr)) return false;
		if (under)
			return IsUnderwaterSector(b) && b->ceilingtexture == a->ceilingtexture && !(b->ceilingstat & CSTAT_SECTOR_SKY) &&
				fabs(b->ceilingz - a->ceilingz) <= 1;
		return !IsUnderwaterSector(b) && b->floortexture == a->floortexture && !(b->floorstat & CSTAT_SECTOR_SKY) &&
			fabs(b->floorz - a->floorz) <= 1;
	};
	std::vector<int> owner(n, -1);
	TArray<int> front, next;
	for (unsigned k = 0; k < copies.Size(); k++)
		for (int si : copies[k].seeds)
			if (owner[si] < 0) { owner[si] = k; front.Push(si); }
	while (front.Size())
	{
		next.Clear();
		for (int si : front)
			for (auto& w : sector[si].walls)
			{
				if (!w.twoSided()) continue;
				auto ns = w.nextSector();
				const int ni = sectindex(ns);
				if (owner[ni] >= 0 || !sameSurface(&sector[si], ns)) continue;
				owner[ni] = owner[si];
				next.Push(ni);
			}
		std::swap(front, next);
	}

	// the renderer makes the copies (with the room-over-room portals of the map)
	for (unsigned k = 0; k < copies.Size(); k++)
	{
		RtCopyLink l;
		l.delta = copies[k].delta;
		l.start = sectindex(copies[k].other);
		l.water = under ? 1 : 2;
		for (unsigned i = 0; i < n; i++)
			if (owner[i] == int(k)) l.holes.push_back(int(i));
		g_rtCopyLinks.push_back(std::move(l));
	}
}

//---------------------------------------------------------------------------
//
// ray tracing: the lights Blood's own effects make (Duke-RT's LIGHTOVR names Duke's;
// Blood has nothing like it, so they come from the game: explosions, burning
// things, flares, fireballs, tesla) and the shotgun's flash
//
//---------------------------------------------------------------------------

extern int gRtShots;

static void RT_BloodLights(DBloodPlayer* pPlayer, sectortype* pSector)
{
	g_rtMuzzleFlash = (float)clamp(pPlayer->visibility / 20., 0., 1.);

	// lightning: Blood's storms flash the light of the outdoor sectors (their lighting
	// effect brightens the shade in bursts, and thunder sounds). A sky sector suddenly
	// brighter than its own recent average -- a sky that pulses slowly is no storm --
	// flashes the path traced sky and the moonlight with it.
	{
		static TArray<float> avg;
		static const void* level = nullptr;
		if (level != (const void*)sector.Data() || avg.Size() != sector.Size())
		{
			level = sector.Data();
			avg.Resize(sector.Size());
			for (unsigned i = 0; i < sector.Size(); i++) avg[i] = -float(sector[i].hasX() ? sector[i].xs().shade : 0);
		}
		float flash = 0.f;
		for (unsigned i = 0; i < sector.Size(); i++)
		{
			auto& sect = sector[i];
			if (!sect.hasX() || !(sect.ceilingstat & CSTAT_SECTOR_SKY)) continue;
			const XSECTOR& x = sect.xs();
			// (a storm's flashes are triggered -- E4M1's -- not an ever running wave: the
			// frozen sky of E2M1 flickers all the time, and is no storm)
			if (!(x.shadeCeiling || x.shadeFloor || x.shadeWalls) || x.shadeAlways) continue;
			const float bright = -float(x.shade);
			flash = max(flash, (bright - avg[i]) / 24.f);
			avg[i] += (bright - avg[i]) * 0.03f;	// about a second
		}
		g_rtSkyFlash = clamp(flash, 0.f, 1.f);
	}
	g_rtCameraUnderwater = pSector && IsUnderwaterSector(pSector);
	g_rtViewerActor = nullptr;
	g_rtSectorSwitch.clear();
	g_rtGameLights.clear();
	g_rtSmokeSources.clear();

	// smoke (rt_smoke.cpp makes it with Duke-RT's styles): bursts once per id, the rest
	// for as long as they are listed
	auto smoke = [](const DVector3& pos, uint64_t id, RtSmokeKind kind, float scale)
	{
		RtSmokeSource s;
		s.pos[0] = pos.X; s.pos[1] = pos.Y; s.pos[2] = pos.Z;
		s.id = id; s.kind = kind; s.scale = scale;
		g_rtSmokeSources.push_back(s);
	};
	static int s_lastShots = 0;
	if (gRtShots != s_lastShots)
	{
		s_lastShots = gRtShots;
		smoke(DVector3(0, 0, 0), uint64_t(gRtShots), RtSmokeKind::Shot, pPlayer->curWeapon == kWeapShotgun ? 1.3f : 0.8f);
	}

	auto add = [](DBloodActor* a, int kind, float r, float g, float b, float lumens, float radius)
	{
		if (!a->insector() || lumens <= 0) return;
		RtGameLight l;
		l.pos[0] = a->spr.pos.X; l.pos[1] = a->spr.pos.Y; l.pos[2] = a->spr.pos.Z;
		l.color[0] = r; l.color[1] = g; l.color[2] = b;
		l.intensity = lumens;
		l.radius = radius;
		l.id = (uint64_t(a->GetIndex()) << 4) | uint64_t(kind);
		l.sector = a->sectno();
		g_rtGameLights.push_back(l);
	};
	// a soft, slow flicker for fire (a jumping light makes the denoiser crawl)
	auto flicker = [](DBloodActor* a)
	{
		const double t = PlayClock * (1. / 120.) * 6.0 + a->GetIndex() * 1.7;
		return float(0.85 + 0.1 * sin(t) + 0.05 * sin(t * 2.3 + 1.0));
	};

	// explosions: the game's own flash strength; the explosion actor lives only a few
	// tics, so its light fades out a little after it
	struct Glow { DVector3 pos; int sector; float peak; int seen; int born; };
	static TMap<int, Glow> s_glow;
	static const void* s_level = nullptr;
	if (s_level != currentLevel) { s_glow.Clear(); s_level = currentLevel; }
	const int now = PlayClock;
	BloodStatIterator it(kStatExplosion);
	while (auto actor = it.Next())
	{
		if (!actor->hasX() || !actor->insector()) continue;
		auto g = s_glow.CheckKey(actor->GetIndex());
		const float peak = 150.f * actor->xspr.data3;
		if (!g) s_glow.Insert(actor->GetIndex(), { actor->spr.pos, actor->sectno(), peak, now, now });
		else { g->pos = actor->spr.pos; g->seen = now; g->peak = max(g->peak, peak); }
		{
			static const float kScale[] = { 0.6f, 1.f, 1.4f, 0.8f, 0.5f, 1.f, 1.f, 1.2f };
			const unsigned t = unsigned(actor->spr.type);
			smoke(actor->spr.pos, uint64_t(actor->GetIndex()), RtSmokeKind::Explosion, t < 8 ? kScale[t] : 1.f);
		}
	}
	{
		TArray<int> gone;
		decltype(s_glow)::Iterator gi(s_glow);
		decltype(s_glow)::Pair* pair;
		while (gi.NextPair(pair))
		{
			Glow& g = pair->Value;
			// full while it burns (a quick swell in), then gone in about half a second
			const float in = clamp((now - g.born) / 6.f, 0.f, 1.f);
			const float t = (now - g.seen) / 60.f;
			if (t >= 1.f) { gone.Push(pair->Key); continue; }
			const float k = (0.4f + 0.6f * in) * (1.f - t) * (1.f - t);
			RtGameLight l;
			l.pos[0] = g.pos.X; l.pos[1] = g.pos.Y; l.pos[2] = g.pos.Z - 16;
			l.color[0] = 1.f; l.color[1] = 0.55f; l.color[2] = 0.22f;
			l.intensity = g.peak * k;
			l.radius = 0.4f;
			l.id = (uint64_t(pair->Key) << 4) | 0;
			l.sector = g.sector;
			g_rtGameLights.push_back(l);
		}
		for (int k : gone) s_glow.Remove(k);
	}
	it.Reset(kStatProjectile);
	while (auto actor = it.Next())
	{
		switch (actor->spr.type)
		{
		case kMissileFlareRegular:
		case kMissileFlareAlt:
			add(actor, 1, 1.f, 0.25f, 0.12f, 700.f, 0.05f);
			smoke(actor->spr.pos, uint64_t(actor->GetIndex()), RtSmokeKind::Trail, 0.4f);
			break;
		case kMissileTeslaRegular:
		case kMissileTeslaAlt:
		case kMissileArcGargoyle:
			add(actor, 1, 0.45f, 0.6f, 1.f, 900.f, 0.05f);
			break;
		case kMissileFireball:
		case kMissileFireballNapalm:
		case kMissileFireballCerberus:
		case kMissileFireballTchernobog:
		case kMissileFlameSpray:
		case kMissileFlameHound:
			add(actor, 1, 1.f, 0.5f, 0.2f, 900.f, 0.1f);
			smoke(actor->spr.pos, uint64_t(actor->GetIndex()), RtSmokeKind::Trail, 0.7f);
			break;
		case kMissileLifeLeechRegular:
		case kMissileLifeLeechAltNormal:
		case kMissileLifeLeechAltSmall:
			add(actor, 1, 0.6f, 1.f, 0.4f, 600.f, 0.05f);
			break;
		default:
			break;
		}
	}
	// life essence and life seed: the hearts glow red
	it.Reset(kStatItem);
	while (auto actor = it.Next())
	{
		if (actor->spr.type == kItemHealthLifeEssense || actor->spr.type == kItemHealthLifeSeed)
		{
			const double t = PlayClock * (1. / 120.) * 3.0 + actor->GetIndex();
			add(actor, 4, 1.f, 0.12f, 0.08f, float(260. * (0.85 + 0.15 * sin(t))), 0.05f);
		}
	}
	// flares stuck in something
	it.Reset(kStatFlare);
	while (auto actor = it.Next())
	{
		add(actor, 2, 1.f, 0.25f, 0.12f, 600.f * flicker(actor), 0.05f);
		smoke(actor->spr.pos, uint64_t(actor->GetIndex()), RtSmokeKind::Trail, 0.3f);
	}
	// burning creatures and things
	for (int stat : { kStatDude, kStatThing })
	{
		it.Reset(stat);
		while (auto actor = it.Next())
		{
			if (actor->hasX() && actor->xspr.burnTime > 0)
			{
				add(actor, 3, 1.f, 0.5f, 0.2f, 500.f * flicker(actor), 0.2f);
				smoke(actor->spr.pos, uint64_t(actor->GetIndex()), RtSmokeKind::Fire, 0.7f);
			}
		}
	}

	// torches, candles, braziers and the other lit decorations: a light of their own. The
	// light from a sprite's glowing area alone (rt_draw.cpp) was faint -- a torch's flame
	// is a small picture. By how much of its picture is fullbright (cached per tile); a
	// lit torch always (its flame is a separate effect). "Candle & torch light" scales it.
	// (Blood's palette has no fullbright colours: its candles, candelabras and lamps are
	// the material table's flame class, their flames found by colour -- the light is by
	// the flame's size, at the flame)
	{
		struct TileLight { float frac; float flamePx; float flameV; };
		static TMap<int, TileLight> s_tileGlow;
		static const void* s_glowLevel = nullptr;
		if (s_glowLevel != currentLevel) { s_tileGlow.Clear(); s_glowLevel = currentLevel; }
		auto tileGlow = [](FTextureID texid) -> TileLight
		{
			if (auto v = s_tileGlow.CheckKey(texid.GetIndex())) return *v;
			TileLight tl{ 0.f, 0.f, 0.25f };
			auto gt = TexMan.GetGameTexture(texid);
			const bool flame = RT_TableFlame(legacyTileNum(texid)) > 0.f;
			if (gt && gt->GetTexture() && (GPalette.HasGlobalBrightmap || flame))
			{
				auto px = gt->GetTexture()->Get8BitPixels(false);
				const int H = gt->GetTexture()->GetHeight();
				const int n = gt->GetTexture()->GetWidth() * H;
				const int white = ColorMatcher.Pick(255, 255, 255);
				int solid = 0, bright = 0, flamePx = 0;
				double rows = 0;
				for (int i = 0; i < n; i++)
				{
					if (px[i] == 255) continue;	// transparent
					solid++;
					if (GPalette.HasGlobalBrightmap) bright += GPalette.GlobalBrightmap.Remap[px[i]] == white;
					const PalEntry c = GPalette.BaseColors[px[i]];
					if (flame && RT_FlameColor(c.r, c.g, c.b))
					{
						flamePx++;
						rows += (i % H) + 0.5;	// (column major)
					}
				}
				// a few speckles are no flame
				tl.frac = solid > 0 && bright >= 6 ? float(bright) / float(solid) : 0.f;
				if (flamePx >= 4)
				{
					tl.flamePx = float(flamePx);
					tl.flameV = float(rows / flamePx / H);
				}
			}
			s_tileGlow.Insert(texid.GetIndex(), tl);
			return tl;
		};
		const float scale = max(0.f, float(*rt_sprite_light_scale)) / 4.5f;
		// glow orbs (624, 625: a white ball the map tints with a palette -- the add-ons'
		// red, blue and yellow lights): a light of the orb's colour, the small ones (a
		// bright dot inside a halo) left to their halo
		auto orbColor = [](DBloodActor* actor, float rgb[3])
		{
			auto gt = TexMan.GetGameTexture(actor->spr.spritetexture());
			int idx = -1, best = -1;
			if (gt && gt->GetTexture())
			{
				auto px = gt->GetTexture()->Get8BitPixels(false);
				const int n = gt->GetTexture()->GetWidth() * gt->GetTexture()->GetHeight();
				for (int i = 0; i < n; i++)
				{
					if (px[i] == 255) continue;
					const PalEntry c = GPalette.BaseColors[px[i]];
					const int l = c.r + c.g + c.b;
					if (l > best) { best = l; idx = px[i]; }
				}
			}
			PalEntry c(255, 255, 255);
			if (idx >= 0)
			{
				c = GPalette.BaseColors[idx];
				if (auto remap = GPalette.GetTranslation(Translation_Remap + curbasepal, actor->spr.pal)) c = remap->Palette[idx];
			}
			const float m = max(1, max(int(c.r), max(int(c.g), int(c.b))));
			rgb[0] = c.r / m; rgb[1] = c.g / m; rgb[2] = c.b / m;
		};
		for (int stat : { kStatDecoration, kStatFX })
		{
		it.Reset(stat);
		while (auto actor = it.Next())
		{
			if (!actor->insector() || (actor->spr.cstat & CSTAT_SPRITE_INVISIBLE) || actor->spr.scale.Y == 0) continue;
			const int tileNo = legacyTileNum(actor->spr.spritetexture());
			if (tileNo == 624 || tileNo == 625)
			{
				const double repeat = actor->spr.scale.X * 64.;
				if (stat != kStatDecoration || repeat < 6) continue;
				RtGameLight l;
				l.pos[0] = actor->spr.pos.X; l.pos[1] = actor->spr.pos.Y; l.pos[2] = actor->spr.pos.Z;
				orbColor(actor, l.color);
				// brighter as it is bigger and as its shade is (-60: as bright as can be)
				const float shade = clamp(1.f - float(actor->spr.shade) / 64.f, 0.3f, 2.f);
				l.intensity = float(clamp(80. * repeat, 300., 2400.)) * shade * scale;
				l.radius = 0.04f;
				l.id = (uint64_t(actor->GetIndex()) << 4) | 6u;
				l.sector = actor->sectno();
				g_rtGameLights.push_back(l);
				continue;
			}
			// (an effect's type is not a decoration's: only flame tiles light up there)
			const bool deco = stat == kStatDecoration;
			const bool torch = deco && (actor->spr.type == kDecorationTorch || (tileNo >= 506 && tileNo <= 509));	// (506: a wall torch, no torch type)
			if (deco && actor->spr.type == kDecorationTorch && actor->hasX() && actor->xspr.state != 1) continue;	// put out
			const TileLight tl = tileGlow(actor->spr.spritetexture());
			float glow = tl.frac;
			if (torch) glow = max(glow, 0.2f);
			// a flame tile: as a torch's (its 49 flame texels) by the flame's size, a
			// candle's about half
			const float flameSize = tl.flamePx * float(actor->spr.scale.X * actor->spr.scale.Y) / 49.f;
			const float flameLight = flameSize > 0.f ? clamp(sqrtf(flameSize), 0.35f, 1.6f) : 0.f;
			if (glow < 0.02f && flameLight == 0.f) continue;

			double top, bottom;
			GetActorExtents(actor, &top, &bottom);
			// the flame: above a torch, else the picture's upper part
			// (506's flame is in its picture, the upper third; drawn as a voxel torch, about there too)
			const bool pictureFlame = tileNo >= 506 && tileNo <= 509;
			// (a flame tile's: where its flame is in the picture -- a candelabra's top, a hanging lamp's bowl)
			const double flameV = pictureFlame ? 0.35 : flameLight > 0.f ? tl.flameV : 0.25;
			DVector3 p(actor->spr.pos.X, actor->spr.pos.Y, torch && !pictureFlame ? top - 4 : top + (bottom - top) * flameV);
			// off the wall it hangs on, into the room: a light on the wall lights the wall
			// (a sprite torch stands in the wall, often a little behind its face: the light goes
			// to the room's side of the nearest wall, whichever side the sprite is on)
			{
				auto sect = actor->sector();
				const bool inRoom = inside(p.X, p.Y, sect) == 1;
				double best = 1e9;
				DVector2 bestC(0, 0), bestD(1, 0);
				for (auto& wal : sect->walls)
				{
					const DVector2 a2 = wal.pos, b2 = wal.point2Wall()->pos, d = b2 - a2;
					const double len2 = d.LengthSquared();
					if (len2 < 1e-6) continue;
					const double t = clamp(((p.XY() - a2) | d) / len2, 0., 1.);
					const DVector2 c = a2 + d * t;
					const double dist = (p.XY() - c).Length();
					if (dist < best) { best = dist; bestC = c; bestD = d; }
				}
				if (best < 1e8 && (!inRoom || best < 10))
				{
					DVector2 n = DVector2(-bestD.Y, bestD.X).Unit();
					if (inside(bestC.X + n.X * 2, bestC.Y + n.Y * 2, sect) != 1) n = -n;
					p = DVector3(bestC + n * 10, p.Z);
				}
			}
			const float lumens = 900.f * max(glow >= 0.02f ? sqrtf(min(glow / 0.2f, 1.5f)) : 0.f, flameLight) * scale * flicker(actor);
			RtGameLight l;
			l.pos[0] = p.X; l.pos[1] = p.Y; l.pos[2] = p.Z;
			l.color[0] = 1.f; l.color[1] = 0.6f; l.color[2] = 0.28f;
			l.intensity = lumens;
			l.radius = 0.06f;
			l.id = (uint64_t(actor->GetIndex()) << 4) | 5u;
			l.sector = actor->sectno();
			g_rtGameLights.push_back(l);
		}
		}
	}
}

void viewDrawScreen(bool sceneonly)
{
	DBloodPlayer* pPlayer = getPlayer(gViewIndex);

	FireProcess();

	double interpfrac;

	if (!paused && (!M_Active() || gGameOptions.nGameType != 0))
	{
		interpfrac = !cl_interpolate || cl_capfps ? 1. : I_GetTimeFrac() * 1.;
	}
	else interpfrac = 1.;

	// update render angles.
	pPlayer->updateCameraAngles(interpfrac);

	if (cl_interpolate)
	{
		DoInterpolations(interpfrac);
	}

	if (automapMode != am_full)
	{
		DoSectorLighting();
	}
	if (automapMode == am_off)
	{
		int basepal = 0;
		if (powerupCheck(pPlayer, kPwUpDeathMask) > 0) basepal = 4;
		else if (powerupCheck(pPlayer, kPwUpReflectShots) > 0) basepal = 1;
		else if (pPlayer->isUnderwater) {
			if (pPlayer->nWaterPal) basepal = pPlayer->nWaterPal;
			else {
				if (pPlayer->GetActor()->xspr.medium == kMediumWater) basepal = 1;
				else if (pPlayer->GetActor()->xspr.medium == kMediumGoo) basepal = 3;
				else basepal = 2;
			}
		}
		UpdateDacs(basepal);
		UpdateBlend(pPlayer);

		DVector3 cPos;
		DRotator cAngles;
		sectortype* pSector;
		double zDelta;
		double shakeX, shakeY;
		SetupView(pPlayer, cPos, cAngles, pSector, zDelta, shakeX, shakeY, interpfrac);

		DAngle tilt = interpolatedvalue(gScreenTiltO, gScreenTilt, interpfrac);
		bool bDelirium = powerupCheck(pPlayer, kPwUpDeliriumShroom) > 0;
		static bool bDeliriumOld = false;
		//int tiltcs, tiltdim;
		uint8_t otherview = powerupCheck(pPlayer, kPwUpCrystalBall) > 0;
		if (tilt.Degrees() || bDelirium)
		{
			cAngles.Roll = -tilt;
		}
		else if (otherview && gNetPlayers > 1)
		{
#if 0    
			renderCrystalBall();
#endif
		}
		else
		{
			othercameraclock = PlayClock + int(4 * interpfrac);
		}

		if (!bDelirium)
		{
			deliriumTilt = 0;
			deliriumTurn = nullAngle;
			deliriumPitch = 0;
		}
		int brightness = 0;

		BloodStatIterator it(kStatExplosion);
		while (auto actor = it.Next())
		{
			if (actor->hasX() && gotsector[actor->sectno()])
			{
				brightness += actor->xspr.data3 * 32;
			}
		}
		it.Reset(kStatProjectile);
		while (auto actor = it.Next())
		{
			switch (actor->spr.type) {
			case kMissileFlareRegular:
			case kMissileTeslaAlt:
			case kMissileFlareAlt:
			case kMissileTeslaRegular:
				if (gotsector[actor->sectno()]) brightness += 256;
				break;
			}
		}
		g_relvisibility = (int32_t)(ClipLow(gVisibility - 32 * pPlayer->visibility - brightness, 0)) - g_visibility;
		cAngles.Yaw += interpolatedvalue(deliriumTurnO, deliriumTurn, interpfrac);

		if (pSector != nullptr)
		{
			double ceilingZ, floorZ;
			calcSlope(pSector, cPos, &ceilingZ, &floorZ);
			if ((cPos.Z > floorZ - 1) && (pSector->upperLink == nullptr)) // clamp to floor
			{
				cPos.Z = floorZ - 1;
			}
			if ((cPos.Z < ceilingZ + 1) && (pSector->lowerLink == nullptr)) // clamp to ceiling
			{
				cPos.Z = ceilingZ + 1;
			}
		}

		if ((tilt.Degrees() || bDelirium) && !sceneonly)
		{
			if (gDeliriumBlur)
			{
				// todo: Set up a blurring postprocessing shader.
				//const float fBlur = pow(1.f/3.f, 30.f/g_frameRate);
				//g lAccum(GL _MULT, fBlur);
				//g lAccum(GL _ACCUM, 1.f-fBlur);
				//g lAccum(GL _RETURN, 1.f);
			}
		}

		if (!sceneonly) hudDraw(pPlayer, pSector, shakeX, shakeY, zDelta, cAngles.Roll, basepal, interpfrac);
		DAngle deliriumPitchI = interpolatedvalue(maphoriz(deliriumPitchO), maphoriz(deliriumPitch), interpfrac);
		auto bakCstat = pPlayer->GetActor()->spr.cstat;
		pPlayer->GetActor()->spr.cstat |= (gViewPos == 0) ? CSTAT_SPRITE_INVISIBLE : CSTAT_SPRITE_TRANSLUCENT | CSTAT_SPRITE_TRANS_FLIP;
		cAngles.Pitch -= deliriumPitchI;
		if (RT_IsActive()) { RT_BloodLights(pPlayer, pSector); RT_BloodWaterCopy(pSector, cPos); }
		render_drawrooms(pPlayer->GetActor(), cPos, pSector, cAngles, interpfrac);
		pPlayer->GetActor()->spr.cstat = bakCstat;
		bDeliriumOld = bDelirium && gDeliriumBlur;

		if (sceneonly) return;
		auto offsets = pPlayer->getCrosshairOffsets(interpfrac);
		DrawCrosshair(pPlayer->GetActor()->xspr.health >> 4, offsets.first.X, offsets.first.Y, 2, offsets.second);
#if 0 // This currently does not work. May have to be redone as a hardware effect.
		if (v4 && gNetPlayers > 1)
		{
			DoLensEffect();
			r otatesprite(IntToFixed(280), IntToFixed(35), 53248, 512, 4077, v10, v14, 512 + 6, gViewX0, gViewY0, gViewX1, gViewY1);
			r otatesprite(IntToFixed(280), IntToFixed(35), 53248, 0, 1683, v10, 0, 512 + 35, gViewX0, gViewY0, gViewX1, gViewY1);
		}
#endif

#if 0
		int tmpSect = nSectnum;
		if ((vf0 & 0xc000) == 0x4000)
		{
			tmpSect = vf0 & (kMaxWalls - 1);
		}
		int v8 = byte_1CE5C2 > 0 && (sector[tmpSect].ceilingstat & CSTAT_SECTOR_SKY);
		if (gWeather.at12d8 > 0 || v8)
		{
			gWeather.Draw(cX, cY, cZ, cAngles.Yaw.Tan() * (1 << 23), cAngles.Pitch.Tan() * (1 << 23) + deliriumPitch, gWeather.at12d8);
			if (v8)
			{
				gWeather.at12d8 = ClipRange(delta * 8 + gWeather.at12d8, 0, 4095);
			}
			else
			{
				gWeather.at12d8 = ClipRange(gWeather.at12d8 - delta * 64, 0, 4095);
			}
		}
#endif
	}
	UpdateDacs(0, true);    // keep the view palette active only for the actual 3D view and its overlays.

	MarkSectorSeen(pPlayer->GetActor()->sector());

	if (automapMode != am_off)
	{
		DrawMap(pPlayer, interpfrac);
	}
	UpdateStatusBar(pPlayer);

	viewDrawAimedPlayerName(pPlayer);
	if (paused)
	{
		auto text = GStrings.GetString("TXTB_PAUSED");
		viewDrawText(PickBigFont(text), text, 160, 10, 0, 0, 1, 0);
	}
	else if (pPlayer->pnum != myconnectindex)
	{
		FStringf gTempStr("] %s [", PlayerName(pPlayer->pnum));
		viewDrawText(OriginalSmallFont, gTempStr.GetChars(), 160, 10, 0, 0, 1, 0);
	}
	if (cl_interpolate)
	{
		RestoreInterpolations();
	}
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

bool GameInterface::GenerateSavePic()
{
	viewDrawScreen(true);
	return true;
}


//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

bool GameInterface::DrawAutomapPlayer(const DVector2& mxy, const DVector2& cpos, const DAngle cang, const DVector2& xydim, const double czoom, double const interpfrac)
{
	auto cangvect = cang.ToVector();

	for (int i = connecthead; i >= 0; i = connectpoint2[i])
	{
		if (i == gViewIndex || gGameOptions.nGameType == 1)
		{
			auto actor = getPlayer(i)->GetActor();
			auto vect = OutAutomapVector(mxy - cpos, cangvect, czoom, xydim);

			DrawTexture(twod, TexMan.GetGameTexture(actor->spr.spritetexture(), true), vect.X, vect.Y, DTA_ClipLeft, viewport3d.Left(), DTA_ClipTop, viewport3d.Top(), DTA_ScaleX, czoom * (2. / 3.), DTA_ScaleY, czoom * (2. / 3.), DTA_CenterOffset, true,
				DTA_ClipRight, viewport3d.Right(), DTA_ClipBottom, viewport3d.Bottom(), DTA_Alpha, (actor->spr.cstat & CSTAT_SPRITE_TRANSLUCENT ? 0.5 : 1.), DTA_TranslationIndex, TRANSLATION(Translation_Remap, actor->spr.pal), TAG_DONE);
		}
	}
	return true;
}

//---------------------------------------------------------------------------
//
//
//
//---------------------------------------------------------------------------

void SerializeView(FSerializer& arc)
{
	if (arc.BeginObject("view"))
	{
		arc("screentilt", gScreenTilt)
			("deliriumtilt", deliriumTilt)
			("deliriumturn", deliriumTurn)
			("deliriumpitch", deliriumPitch)
			.EndObject();
	}
}


END_BLD_NS
