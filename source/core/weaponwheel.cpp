//-------------------------------------------------------------------------
//
// The weapon wheel (Raze RT): hold its key (bind <key> +weaponwheel) and the weapons the
// player has are laid out round a wheel -- and his inventory items on a ring inside it --
// the game slowed down meanwhile; the mouse points at one, letting go of the key takes
// the weapon or uses the item. Blood and Duke give the lists and make the pick
// (GameInterface::WeaponWheelList / WeaponWheelItems / WeaponWheelSelect; an item is
// used as its own key would, useitem).
//
//-------------------------------------------------------------------------

#include <math.h>
#include "c_cvars.h"
#include "c_dispatch.h"
#include "gamestruct.h"
#include "gamecontrol.h"
#include "gamestate.h"
#include "menu.h"
#include "v_draw.h"
#include "v_font.h"
#include "v_video.h"
#include "texturemanager.h"
#include "i_time.h"
#include "i_net.h"
#include "weaponwheel.h"

CVARD(Float, weaponwheel_slowdown, 0.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "weapon wheel: the game's speed while it is open (1 = no slowdown, 0.05 = slowest)")
CVARD(Float, weaponwheel_size, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "weapon wheel: its size on screen")
CVARD(Float, weaponwheel_sensitivity, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "weapon wheel: how far the mouse moves to point at a weapon (higher = less)")
EXTERN_CVAR(Float, i_timescale)

static bool s_open = false;
static FVector2 s_cursor;
static int s_sel = -1;        // the weapon pointed at
static int s_selItem = -1;    // or the item
static TArray<WheelWeapon> s_items, s_inv;
static float s_prevScale = 1.f;

// the mouse's travel (its own units, before weaponwheel_sensitivity): the dead middle,
// the items' ring up to here, the weapons' beyond, as far as it goes
constexpr float kDead = 26.f, kInner = 86.f, kMax = 120.f;

static bool CanOpen()
{
	return gamestate == GS_LEVEL && !M_Active() && !paused && !netgame && gi;
}

static void WheelOpen()
{
	if (s_open || !CanOpen()) return;
	s_items.Clear();
	s_inv.Clear();
	gi->WeaponWheelList(s_items);
	gi->WeaponWheelItems(s_inv);
	if (s_items.Size() == 0 && s_inv.Size() == 0) return;
	s_open = true;
	s_cursor.Zero();
	s_sel = s_selItem = -1;
	s_prevScale = i_timescale;
	const float slow = clamp(float(*weaponwheel_slowdown), 0.05f, 1.f);
	if (slow < 1.f && fabs(slow - s_prevScale) > 0.001f) i_timescale = slow;
}

static void WheelClose(bool take)
{
	if (!s_open) return;
	s_open = false;
	if (fabs(float(i_timescale) - s_prevScale) > 0.001f) i_timescale = s_prevScale >= 0.05f ? s_prevScale : 1.f;
	if (take && gi && CanOpen())
	{
		if (s_selItem >= 0 && unsigned(s_selItem) < s_inv.Size())
		{
			// (an item in use is switched off the same way: its key toggles it)
			C_DoCommand(FStringf("useitem %d", s_inv[s_selItem].id).GetChars());
		}
		else if (s_sel >= 0 && unsigned(s_sel) < s_items.Size() && !s_items[s_sel].current)
		{
			gi->WeaponWheelSelect(s_items[s_sel].id);
		}
	}
	s_items.Clear();
	s_inv.Clear();
}

static FConsoleCommand s_wheelOn("+weaponwheel", [](FCommandLine&, int) { WheelOpen(); });
static FConsoleCommand s_wheelOff("-weaponwheel", [](FCommandLine&, int) { WheelClose(true); });

bool WeaponWheel_Active()
{
	return s_open;
}

static int Sector(const FVector2& c, unsigned n)
{
	// the first at the top, the rest clockwise (screen: x right, y up here)
	double a = atan2(c.X, c.Y); // 0 = up, clockwise positive
	if (a < 0) a += 2 * M_PI;
	const double step = 2 * M_PI / n;
	return int(floor(a / step + 0.5)) % int(n);
}

// the mouse while the wheel is open: it points, it does not turn the view
void WeaponWheel_Mouse(const FVector2& delta)
{
	if (!s_open) return;
	s_cursor += delta / std::max(0.1f, float(*weaponwheel_sensitivity));
	const float len0 = s_cursor.Length();
	if (len0 > kMax) s_cursor *= kMax / len0;
	const float len = std::min(len0, kMax);
	if (len <= kDead) return; // (in the middle: the last pick stands)
	const bool inner = s_inv.Size() > 0 && (len < kInner || s_items.Size() == 0);
	if (inner)
	{
		s_selItem = Sector(s_cursor, s_inv.Size());
		s_sel = -1;
	}
	else if (s_items.Size() > 0)
	{
		s_sel = Sector(s_cursor, s_items.Size());
		s_selItem = -1;
	}
}

// one weapon's or item's box: its picture (or name) and its count
static void DrawEntry(const WheelWeapon& it, double x, double y, double B, bool sel, FFont* font, double ts, int H)
{
	const int bx = int(x - B * 0.5), by = int(y - B * 0.5), bw = int(B), bh = int(B);
	Dim(twod, sel ? PalEntry(255, 255, 170, 60) : PalEntry(255, 0, 0, 0), sel ? 0.45f : 0.55f, bx, by, bw, bh);
	const uint32_t edge = sel ? 0xffffc040 : it.current ? 0xffc0c0c0 : 0xff505050;
	const double th = sel ? std::max(2.0, H / 300.0) : std::max(1.0, H / 600.0);
	twod->AddThickLine(DVector2(bx, by), DVector2(bx + bw, by), th, edge);
	twod->AddThickLine(DVector2(bx + bw, by), DVector2(bx + bw, by + bh), th, edge);
	twod->AddThickLine(DVector2(bx + bw, by + bh), DVector2(bx, by + bh), th, edge);
	twod->AddThickLine(DVector2(bx, by + bh), DVector2(bx, by), th, edge);

	// the pickup's picture, as big as fits, its proportions kept
	auto tex = it.icon.isValid() ? TexMan.GetGameTexture(it.icon, true) : nullptr;
	if (tex && tex->GetDisplayWidth() > 0 && tex->GetDisplayHeight() > 0)
	{
		const double inner = B * 0.72;
		const double tw = tex->GetDisplayWidth(), tht = tex->GetDisplayHeight();
		const double k = std::min(inner / tw, inner / tht);
		const double dw = tw * k, dh = tht * k;
		DrawTexture(twod, tex, x - dw * 0.5, y - dh * 0.5 - B * 0.06, DTA_DestWidthF, dw, DTA_DestHeightF, dh,
			DTA_LeftOffset, 0, DTA_TopOffset, 0, DTA_Alpha, it.empty ? 0.4 : 1.0, TAG_DONE);
	}
	else
	{
		const double sw = font->StringWidth(it.name) * ts * 0.8;
		DrawText(twod, font, it.empty ? CR_DARKGRAY : CR_WHITE, x - sw * 0.5, y - font->GetHeight() * ts * 0.4,
			it.name.GetChars(), DTA_ScaleX, ts * 0.8, DTA_ScaleY, ts * 0.8, TAG_DONE);
	}
	if (it.ammo >= 0)
	{
		FString am;
		if (it.percent) am.Format("%d%%", it.ammo);
		else am.Format("%d", it.ammo);
		const double sw = font->StringWidth(am) * ts * 0.7;
		DrawText(twod, font, it.empty ? CR_RED : it.current && it.percent ? CR_GREEN : CR_GOLD, x - sw * 0.5,
			y + B * 0.5 - font->GetHeight() * ts * 0.75, am.GetChars(), DTA_ScaleX, ts * 0.7, DTA_ScaleY, ts * 0.7, TAG_DONE);
	}
}

void WeaponWheel_Draw()
{
	if (!s_open) return;
	if (!CanOpen())
	{
		WheelClose(false);
		return;
	}
	const int W = twod->GetWidth(), H = twod->GetHeight();
	const double size = clamp(double(*weaponwheel_size), 0.4, 2.0);
	const double cx = W * 0.5, cy = H * 0.5;
	const double R = H * 0.30 * size;      // the weapons' ring
	const double B = H * 0.13 * size;      // a weapon's box
	const double Bi = B * 0.78;            // an item's

	Dim(twod, 0, 0.35f, 0, 0, W, H);

	FFont* font = NewSmallFont ? NewSmallFont : SmallFont;
	const double ts = std::max(1.0, H / 540.0) * size;

	for (unsigned i = 0; i < s_items.Size(); i++)
	{
		const double a = 2 * M_PI * i / s_items.Size();
		DrawEntry(s_items[i], cx + sin(a) * R, cy - cos(a) * R, B, int(i) == s_sel, font, ts, H);
	}
	const double ri = s_inv.Size() ? R * 0.47 : 0;
	for (unsigned i = 0; i < s_inv.Size(); i++)
	{
		const double a = 2 * M_PI * i / s_inv.Size();
		DrawEntry(s_inv[i], cx + sin(a) * ri, cy - cos(a) * ri, Bi, int(i) == s_selItem, font, ts * 0.85, H);
	}

	// the name of what is pointed at: in the middle, or (with the items' ring there)
	// under the wheel
	const WheelWeapon* shown = s_selItem >= 0 ? &s_inv[s_selItem] : s_sel >= 0 ? &s_items[s_sel] : nullptr;
	if (shown)
	{
		const double k = 1.2;
		const double sw = font->StringWidth(shown->name) * ts * k;
		const double ty = s_inv.Size() ? cy + R + B * 0.62 : cy - font->GetHeight() * ts * k * 0.5;
		DrawText(twod, font, CR_GOLD, cx - sw * 0.5, ty, shown->name.GetChars(), DTA_ScaleX, ts * k, DTA_ScaleY, ts * k, TAG_DONE);
	}
	// and where the mouse points (its travel drawn to the rings: the items' ring at
	// the middle of its zone, the weapons' at the end)
	auto toScreen = [&](float len) -> double
	{
		const double zi = (kDead + kInner) * 0.5;
		if (s_inv.Size() == 0) return len / kMax * R;
		if (len <= zi) return len / zi * ri;
		return ri + (len - zi) / (kMax - zi) * (R - ri);
	};
	const float len = s_cursor.Length();
	const double pr = toScreen(len);
	const double px = len > 0 ? cx + s_cursor.X / len * pr : cx, py = len > 0 ? cy - s_cursor.Y / len * pr : cy;
	const double d = std::max(3.0, H / 220.0);
	twod->AddThickLine(DVector2(px - d, py), DVector2(px + d, py), std::max(1.0, H / 500.0), 0xffffffff);
	twod->AddThickLine(DVector2(px, py - d), DVector2(px, py + d), std::max(1.0, H / 500.0), 0xffffffff);
}
