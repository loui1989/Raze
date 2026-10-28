#pragma once
#include "vectors.h"

// the weapon wheel (core/weaponwheel.cpp)
bool WeaponWheel_Active();
void WeaponWheel_Mouse(const FVector2& delta);
void WeaponWheel_Draw();
