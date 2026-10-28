//-------------------------------------------------------------------------
//
// The weapon wheel (Raze RT): hold its key (bind <key> +weaponwheel) and the weapons the
// player has are laid out round a wheel, the game slowed down meanwhile; the mouse
// points at one, letting go of the key takes it. Blood and Duke give the list and make
// the pick (GameInterface::WeaponWheelList / WeaponWheelSelect).
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
static int s_sel = -1;
static TArray<WheelWeapon> s_items;
static float s_prevScale = 1.f;

static bool CanOpen()
{
	return gamestate == GS_LEVEL && !M_Active() && !paused && !netgame && gi;
}

static void WheelOpen()
{
	if (s_open || !CanOpen()) return;
	s_items.Clear();
	gi->WeaponWheelList(s_items);
	if (s_items.Size() == 0) return;
	s_open = true;
	s_cursor.Zero();
	s_sel = -1;
	s_prevScale = i_timescale;
	const float slow = clamp(float(*weaponwheel_slowdown), 0.05f, 1.f);
	if (slow < 1.f && fabs(slow - s_prevScale) > 0.001f) i_timescale = slow;
}

static void WheelClose(bool take)
{
	if (!s_open) return;
	s_open = false;
	if (fabs(float(i_timescale) - s_prevScale) > 0.001f) i_timescale = s_prevScale >= 0.05f ? s_prevScale : 1.f;
	if (take && s_sel >= 0 && unsigned(s_sel) < s_items.Size() && gi && CanOpen())
	{
		if (!s_items[s_sel].current) gi->WeaponWheelSelect(s_items[s_sel].id);
	}
	s_items.Clear();
}

static FConsoleCommand s_wheelOn("+weaponwheel", [](FCommandLine&, int) { WheelOpen(); });
static FConsoleCommand s_wheelOff("-weaponwheel", [](FCommandLine&, int) { WheelClose(true); });

bool WeaponWheel_Active()
{
	return s_open;
}

// the mouse while the wheel is open: it points, it does not turn the view
void WeaponWheel_Mouse(const FVector2& delta)
{
	if (!s_open) return;
	s_cursor += delta / std::max(0.1f, float(*weaponwheel_sensitivity));
	const float maxLen = 120.f;
	const float len = s_cursor.Length();
	if (len > maxLen) s_cursor *= maxLen / len;
	if (len > 28.f && s_items.Size() > 0)
	{
		// the first item at the top, the rest clockwise (screen: x right, y up here)
		double a = atan2(s_cursor.X, s_cursor.Y); // 0 = up, clockwise positive
		if (a < 0) a += 2 * M_PI;
		const double step = 2 * M_PI / s_items.Size();
		s_sel = int(floor(a / step + 0.5)) % int(s_items.Size());
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
	const double R = H * 0.30 * size;
	const double B = H * 0.13 * size;   // an item's box
	const unsigned n = s_items.Size();

	Dim(twod, 0, 0.35f, 0, 0, W, H);

	FFont* font = NewSmallFont ? NewSmallFont : SmallFont;
	const double ts = std::max(1.0, H / 540.0) * size;

	for (unsigned i = 0; i < n; i++)
	{
		const auto& it = s_items[i];
		const double a = 2 * M_PI * i / n;
		const double x = cx + sin(a) * R, y = cy - cos(a) * R;
		const bool sel = int(i) == s_sel;
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
			am.Format("%d", it.ammo);
			const double sw = font->StringWidth(am) * ts * 0.7;
			DrawText(twod, font, it.empty ? CR_RED : CR_GOLD, x - sw * 0.5, y + B * 0.5 - font->GetHeight() * ts * 0.75,
				am.GetChars(), DTA_ScaleX, ts * 0.7, DTA_ScaleY, ts * 0.7, TAG_DONE);
		}
	}

	// the middle: the weapon pointed at
	const int shown = s_sel >= 0 ? s_sel : -1;
	if (shown >= 0)
	{
		const auto& it = s_items[shown];
		const double sw = font->StringWidth(it.name) * ts * 1.2;
		DrawText(twod, font, CR_GOLD, cx - sw * 0.5, cy - font->GetHeight() * ts * 0.6, it.name.GetChars(),
			DTA_ScaleX, ts * 1.2, DTA_ScaleY, ts * 1.2, TAG_DONE);
	}
	// and where the mouse points
	const double px = cx + s_cursor.X / 120. * R * 0.55, py = cy - s_cursor.Y / 120. * R * 0.55;
	const double d = std::max(3.0, H / 220.0);
	twod->AddThickLine(DVector2(px - d, py), DVector2(px + d, py), std::max(1.0, H / 500.0), 0xffffffff);
	twod->AddThickLine(DVector2(px, py - d), DVector2(px, py + d), std::max(1.0, H / 500.0), 0xffffffff);
}
