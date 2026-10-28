//-------------------------------------------------------------------------
/*
Copyright (C) 2010-2019 EDuke32 developers and contributors
Copyright (C) 2019 Nuke.YKT
Copyright (C) 2020 Raze developers and contributors

This file is part of Raze.

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

#include "build.h"

#include "blood.h"
#include "mapinfo.h"
#include "gamestate.h"
#include "gameinput.h"

BEGIN_BLD_NS

void GameInterface::ToggleThirdPerson()
{
	if (gamestate != GS_LEVEL) return;
	if (gViewPos > viewFirstPerson)
	{
		gViewPos = viewFirstPerson;
	}
	else
	{
		gViewPos = viewThirdPerson;
		cameradist = 0;
		cameraclock = INT_MIN;
	}
}

void GameInterface::SwitchCoopView()
{
	if (gamestate != GS_LEVEL) return;
	if (gGameOptions.nGameType == 1)
	{
		gViewIndex = connectpoint2[gViewIndex];
		if (gViewIndex == -1)
			gViewIndex = connecthead;
	}
	else if (gGameOptions.nGameType == 3)
	{
		int oldViewIndex = gViewIndex;
		do
		{
			gViewIndex = connectpoint2[gViewIndex];
			if (gViewIndex == -1)
				gViewIndex = connecthead;
			if (oldViewIndex == gViewIndex || getPlayer(myconnectindex)->teamId == getPlayer(gViewIndex)->teamId)
				break;
		} while (oldViewIndex != gViewIndex);
	}
}

void GameInterface::ToggleShowWeapon()
{
	if (gamestate != GS_LEVEL) return;
	cl_showweapon = (cl_showweapon + 1) & 3;
}

//---------------------------------------------------------------------------
//
// the weapon wheel (core/weaponwheel.cpp): Blood's weapons by their numbers
//
//---------------------------------------------------------------------------

void GameInterface::WeaponWheelList(TArray<WheelWeapon>& out)
{
	if (myconnectindex < 0) return;
	DBloodPlayer* pPlayer = getPlayer(myconnectindex);
	if (!pPlayer || !pPlayer->GetActor()) return;
	static const char* const names[] = { "", "Pitchfork", "Flare Pistol", "Sawed-Off", "Tommy Gun", "Napalm Launcher", "Dynamite",
		"Spray Can", "Tesla Cannon", "Life Leech", "Voodoo Doll", "Proximity Bomb", "Remote Bomb" };
	for (int w = kWeapPitchFork; w <= kWeapRemote; w++)
	{
		if (!pPlayer->hasWeapon[w]) continue;
		WheelWeapon ww;
		ww.id = w;
		ww.name = names[w];
		// its pickup's picture (gWeaponItemData's tiles; the bombs: the dynamite's)
		static const int pickup[] = { -1, -1, 524, 559, 558, 526, 589, 618, 539, 800, 525, 589, 589 };
		if (pickup[w] >= 0) ww.icon = tileGetTextureID(pickup[w]);
		if (w != kWeapPitchFork)
		{
			const int amType = w - 1;
			ww.ammo = pPlayer->ammoCount[amType] / (amType == 6 ? 10 : 1);
			ww.empty = pPlayer->ammoCount[amType] <= 0;
		}
		ww.current = pPlayer->curWeapon == w;
		out.Push(ww);
	}
}

void GameInterface::WeaponWheelSelect(int id)
{
	if (id >= kWeapPitchFork && id <= kWeapRemote) gameInput.SendWeapon(id);
}

END_BLD_NS
