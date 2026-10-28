//-------------------------------------------------------------------------
/*
Copyright (C) 1996, 2003 - 3D Realms Entertainment
Copyright 2020 Christoph Oelckers

This file is part of Duke Nukem 3D version 1.5 - Atomic Edition

Duke Nukem 3D is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

Original Source: 1996 - Todd Replogle
Prepared for public release: 03/21/2003 - Charlie Wiederhold, 3D Realms
Modifications for JonoF's port by Jonathon Fowler (jf@jonof.id.au)
*/
//-------------------------------------------------------------------------

#include "ns.h"

#include "duke3d.h"
#include "mapinfo.h"
#include "cheathandler.h"
#include "c_dispatch.h"
#include "gamestate.h"
#include "gamefuncs.h"
#include "dukeactor.h"
#include "gameinput.h"
#include "texturemanager.h"

BEGIN_DUKE_NS

int getlabelvalue(const char* text);

static int ccmd_spawn(CCmdFuncPtr parm)
{
	FTextureID texid = FNullTextureID();
	int picno = -1;
	int x = 0, y = 0, z = 0;
	ESpriteFlags cstat = 0;
	PClassActor* cls = nullptr;
	unsigned int pal = 0;
	DAngle ang = nullAngle;
	int set = 0;

#if 0 // fixme - route through the network and this limitation becomes irrelevant
	if (netgame || numplayers > 1 || !(ps[myconnectindex].gm & MODE_GAME)) {
		Printf("spawn: Can't spawn sprites in multiplayer games or demos\n");
		return CCMD_OK;
	}
#endif

	switch (parm->numparms) {
	case 7: // x,y,z
		x = atol(parm->parms[4]);
		y = atol(parm->parms[5]);
		z = atol(parm->parms[6]);
		set |= 8;
		[[fallthrough]];
	case 4: // ang
		ang = DAngle::fromDeg(atoi(parm->parms[3])); set |= 4;
		[[fallthrough]];
	case 3: // cstat
		cstat = ESpriteFlags::FromInt(atoi(parm->parms[2])); set |= 2;
		[[fallthrough]];
	case 2: // pal
		pal = (uint8_t)atol(parm->parms[1]); set |= 1;
		[[fallthrough]];
	case 1: // tile number
		if (isdigit((uint8_t)parm->parms[0][0])) {
			picno = (unsigned short)atol(parm->parms[0]);
			cls = GetSpawnType(picno);
		}
		else 
		{
			cls = PClass::FindActor(parm->parms[0]);
			if (!cls)
			{
				texid = TexMan.CheckForTexture(parm->parms[0], ETextureType::Any, FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ReturnAll);
				if (texid.isValid())
				{
					picno = legacyTileNum(texid);
				}
				if (picno < 0)
				{
					picno = getlabelvalue(parm->parms[0]);
				}
				cls = GetSpawnType(picno);
			}
		}

		if (cls == nullptr && !texid.isValid())
		{
			Printf("spawn: Invalid actor type '%s'\n", parm->parms[0]);
			return CCMD_OK;
		}
		break;
	default:
		return CCMD_SHOWHELP;
	}

	const auto pact = getPlayer(myconnectindex)->GetActor();
	if (DDukeActor* spawned = !cls ? spawnsprite(pact, picno) : spawn(pact, cls))
	{
		if (set & 1) spawned->spr.pal = (uint8_t)pal;
		if (set & 2) spawned->spr.cstat = ESpriteFlags::FromInt(cstat);
		if (set & 4) spawned->spr.Angles.Yaw = ang;
		if (set & 8) SetActor(spawned, DVector3( x, y, z ));
		if (spawned->spr.scale.isZero()) spawned->spr.scale = isRR() ? DVector2(0.5, 0.5) : DVector2(1., 1.); // nake sure it's visible.

		if (spawned->sector() == nullptr)
		{
			Printf("spawn: Sprite cannot be spawned into null space\n");
			spawned->Destroy();
		}
	}

	return CCMD_OK;
}

void GameInterface::ToggleThirdPerson()
{
	if (gamestate != GS_LEVEL) return;
	const auto p = getPlayer(myconnectindex);
	if (!isRRRA() || (!p->OnMotorcycle && !p->OnBoat))
	{
		if (p->over_shoulder_on)
			p->over_shoulder_on = 0;
		else
		{
			p->over_shoulder_on = 1;
			cameradist = 0;
			cameraclock = INT_MIN;
		}
		FTA(QUOTE_VIEW_MODE_OFF + p->over_shoulder_on, p);
	}
}

void GameInterface::SwitchCoopView()
{
	if (gamestate != GS_LEVEL) return;
	if (ud.coop || ud.recstat == 2)
	{
		screenpeek = connectpoint2[screenpeek];
		if (screenpeek == -1) screenpeek = 0;
	}
}

void GameInterface::ToggleShowWeapon()
{
	if (gamestate != GS_LEVEL) return;
	cl_showweapon = cl_showweapon == 0;
	FTA(QUOTE_WEAPON_MODE_OFF - cl_showweapon, getPlayer(screenpeek));
}

//---------------------------------------------------------------------------
//
// the weapon wheel (core/weaponwheel.cpp): Duke's weapons by their number keys, the
// expander and the incinerator after the weapon they share a key with
//
//---------------------------------------------------------------------------

void GameInterface::WeaponWheelList(TArray<WheelWeapon>& out)
{
	if (isRR() || myconnectindex < 0) return;
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor()) return;
	static const struct { int w; const char* icon; const char* name; } order[] = {
		{ KNEE_WEAPON, "KNEE", "Mighty Foot" },
		{ PISTOL_WEAPON, "FIRSTGUNSPRITE", "Pistol" },
		{ SHOTGUN_WEAPON, "SHOTGUNSPRITE", "Shotgun" },
		{ CHAINGUN_WEAPON, "CHAINGUNSPRITE", "Ripper" },
		{ RPG_WEAPON, "RPGSPRITE", "RPG" },
		{ HANDBOMB_WEAPON, "HEAVYHBOMB", "Pipe Bomb" },
		{ SHRINKER_WEAPON, "SHRINKERSPRITE", "Shrinker" },
		{ GROW_WEAPON, "GROWSPRITEICON", "Expander" },
		{ DEVISTATOR_WEAPON, "DEVISTATORSPRITE", "Devastator" },
		{ TRIPBOMB_WEAPON, "TRIPBOMBSPRITE", "Laser Tripbomb" },
		{ FREEZE_WEAPON, "FREEZESPRITE", "Freezethrower" },
		{ FLAMETHROWER_WEAPON, "FLAMETHROWERSPRITE", "Incinerator" },
	};
	for (auto& o : order)
	{
		if (o.w == GROW_WEAPON && !isPlutoPak()) continue;
		if (o.w == FLAMETHROWER_WEAPON && !isWorldTour()) continue;
		if (!p->gotweapon[o.w]) continue;
		WheelWeapon ww;
		ww.id = o.w;
		ww.icon = TexMan.CheckForTexture(o.icon, ETextureType::Any);
		ww.name = o.name;
		ww.ammo = o.w == KNEE_WEAPON ? -1 : p->ammo_amount[o.w];
		ww.current = p->curr_weapon == o.w || (o.w == HANDBOMB_WEAPON && p->curr_weapon == HANDREMOTE_WEAPON);
		ww.empty = o.w != KNEE_WEAPON && p->ammo_amount[o.w] <= 0;
		out.Push(ww);
	}
}

void GameInterface::WeaponWheelItems(TArray<WheelWeapon>& out)
{
	if (isRR() || myconnectindex < 0) return;
	auto p = getPlayer(myconnectindex);
	if (!p || !p->GetActor()) return;
	// (useitem's numbers; amounts as the status bar gives them)
	struct It { int use; const char* icon; const char* name; int amount; int percent; bool on; };
	const It items[] = {
		{ 1, "FIRSTAID", "Portable Medkit", p->firstaid_amount, p->firstaid_amount, false },
		{ 2, "STEROIDS", "Steroids", p->steroids_amount, (p->steroids_amount + 3) >> 2, false },
		{ 3, "HOLODUKE", "Holoduke", p->holoduke_amount, (p->holoduke_amount + 15) / 24, p->holoduke_on != nullptr },
		{ 4, "JETPACK", "Jetpack", p->jetpack_amount, (p->jetpack_amount + 15) >> 4, !!p->jetpack_on },
		{ 5, "HEATSENSOR", "Night Vision", p->heat_amount, p->heat_amount / 12, !!p->heat_on },
	};
	for (auto& i : items)
	{
		if (i.amount <= 0) continue;
		WheelWeapon ww;
		ww.id = i.use;
		ww.icon = TexMan.CheckForTexture(i.icon, ETextureType::Any);
		ww.name = i.name;
		ww.ammo = i.percent;
		ww.percent = true;
		ww.current = i.on;
		out.Push(ww);
	}
}

void GameInterface::WeaponWheelSelect(int id)
{
	auto p = getPlayer(myconnectindex);
	if (!p) return;
	switch (id)
	{
	case SHRINKER_WEAPON:
		// (the shrinker's key gives the expander when it was the last one used)
		if (p->curr_weapon != GROW_WEAPON) p->subweapon &= ~(1 << GROW_WEAPON);
		gameInput.SendWeapon(SHRINKER_WEAPON + 1);
		break;
	case GROW_WEAPON:
		if (p->curr_weapon != SHRINKER_WEAPON) p->subweapon |= (1 << GROW_WEAPON);
		gameInput.SendWeapon(SHRINKER_WEAPON + 1);
		break;
	case FREEZE_WEAPON:
		if (p->curr_weapon != FLAMETHROWER_WEAPON) p->subweapon &= ~(1 << FLAMETHROWER_WEAPON);
		gameInput.SendWeapon(FREEZE_WEAPON + 1);
		break;
	case FLAMETHROWER_WEAPON:
		if (p->curr_weapon != FREEZE_WEAPON) p->subweapon |= (1 << FLAMETHROWER_WEAPON);
		gameInput.SendWeapon(FREEZE_WEAPON + 1);
		break;
	default:
		if (id >= KNEE_WEAPON && id <= FREEZE_WEAPON) gameInput.SendWeapon(id + 1);
		break;
	}
}

bool GameInterface::WantEscape() 
{ 
	return getPlayer(myconnectindex)->newOwner != nullptr;
}


int registerosdcommands(void)
{
	C_RegisterFunction("spawn","spawn <typename> [palnum] [cstat] [ang] [x y z]: spawns a sprite with the given properties",ccmd_spawn);
	return 0;
}

END_DUKE_NS
