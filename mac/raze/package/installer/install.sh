#!/bin/bash
# Raze RT installer: what "Install Raze RT.app" runs.
#
# Finds the games' files on this Mac (Duke Nukem 3D: DUKE3D.GRP, Blood: BLOOD.RFF),
# asks which games to install, finds the optional extras the player has downloaded
# (voxel models, the Blood Upscale Pack, Death Wish, Marrow) and asks which of those to
# take, then installs:
#   ~/Library/Application Support/Raze RT/Engine   the engine and the path tracer
#   ~/Library/Application Support/Raze RT/Duke     Duke's settings, saves, logs, extras
#   ~/Library/Application Support/Raze RT/Blood    Blood's
#   /Applications/Duke Nukem 3D RT.app, Blood RT.app   one launcher per game
#                                        (~/Applications if /Applications is not writable)
# Running it again updates the engine and keeps saves, settings and extras.
#
# Nothing of the games is copied: the launchers read the files where they are.

RES="$(cd "$(dirname "$0")/../Resources" && pwd)"
PAYLOAD="$RES/Payload"
DEST="${RAZE_RT_DEST:-$HOME/Library/Application Support/Raze RT}"
OSASCRIPT="${OSASCRIPT:-/usr/bin/osascript}"
DITTO="${DITTO:-/usr/bin/ditto}"
TITLE="Raze RT"
TILDE="~"   # (for shortening paths: a ~ written in ${x/#$HOME/~} is expanded by bash 3.2)
LOG="$(mktemp "${TMPDIR:-/tmp}/razert-install.XXXXXX")"

# --- dialogs (AppleScript, its text given as arguments: no quoting trouble) -----------

osa() { "$OSASCRIPT" - "$@"; }

# message <text> [button...]: the button pressed (the last one is the default)
message() {
  osa "$TITLE" "$@" <<'OSA'
on run argv
  set b to {"OK"}
  if (count of argv) > 2 then set b to items 3 thru -1 of argv
  activate
  set r to display dialog (item 2 of argv) with title (item 1 of argv) buttons b default button (count of b)
  return button returned of r
end run
OSA
}

# pick <prompt> <multiple 0|1> <preselected...> -- <items...>: the chosen items, one per
# line (nothing if none), or __quit__
pick() {
  local out
  out="$(pick_osa "$@" 2>>"$LOG")" || { fail_dialog; return; }
  printf '%s\n' "$out"
}
fail_dialog() {
  osa "$TITLE" "The installer could not show its list (an AppleScript error). The details are in $LOG." <<'OSA' >/dev/null 2>&1
on run argv
  activate
  display dialog (item 2 of argv) with title (item 1 of argv) buttons {"OK"} default button 1 with icon stop
end run
OSA
  echo "__quit__"
}
pick_osa() {
  osa "$TITLE" "$@" <<'OSA'
on run argv
  set p to item 2 of argv
  set m to (item 3 of argv) is "1"
  set pre to {}
  set choices to {}
  set seenSep to false
  repeat with i from 4 to count of argv
    set a to item i of argv
    if a is "--" then
      set seenSep to true
    else if seenSep then
      set end of choices to a
    else
      set end of pre to a
    end if
  end repeat
  activate
  set c to choose from list choices with title (item 1 of argv) with prompt p default items pre multiple selections allowed m empty selection allowed true OK button name "Continue" cancel button name "Quit"
  if c is false then return "__quit__"
  set AppleScript's text item delimiters to linefeed
  return c as text
end run
OSA
}

choose_folder() {
  osa "$1" <<'OSA'
on run argv
  activate
  try
    return POSIX path of (choose folder with prompt (item 1 of argv))
  on error
    return ""
  end try
end run
OSA
}

notify() {
  osa "$TITLE" "$1" <<'OSA' >/dev/null 2>&1
on run argv
  display notification (item 2 of argv) with title (item 1 of argv)
end run
OSA
}

fail() {
  message "$1

(The details are in $LOG.)" "OK" >/dev/null
  exit 1
}

# --- where things are ----------------------------------------------------------------

# the folders under <roots> with a file named <name> (any case), nearest first
dirs_with() {
  local name="$1"; shift
  local r
  for r in "$@"; do
    [ -d "$r" ] || continue
    find "$r" -maxdepth 5 \( -name '.Trash' -o -name 'Raze RT' -o -name 'Xcode*.app' -o -name '*.photoslibrary' \) -prune \
      -o -type f -iname "$name" -print 2>/dev/null
  done | while IFS= read -r f; do dirname "$f"; done | awk '!seen[$0]++'
}

# a file in a folder, by name in any case: its path
file_in() { find "$1" -maxdepth 1 -type f -iname "$2" 2>/dev/null | head -1; }

DOWNLOADS="$HOME/Downloads"
GAME_ROOTS=("$HOME/Games" "$HOME/Library/Application Support/Steam/steamapps/common"
            "$HOME/Library/Application Support/Steam/SteamApps/common" "$HOME/Applications"
            "/Applications" "$DOWNLOADS")

# --- start ---------------------------------------------------------------------------

[ -d "$PAYLOAD/Engine/raze.app" ] || fail "This installer is incomplete (its Payload is missing)."

if [ "$(uname -m)" != arm64 ]; then
  message "Raze RT needs a Mac with Apple silicon (M1 or later). This Mac has an Intel processor." "Quit" >/dev/null
  exit 1
fi
MACOS="$(sw_vers -productVersion 2>/dev/null)"
if [ "${MACOS%%.*}" -lt 14 ] 2>/dev/null; then
  [ "$(message "Raze RT was made for macOS 14 or newer; this Mac has macOS $MACOS. It may not run." "Quit" "Install anyway")" = "Install anyway" ] || exit 0
fi

r="$(message "Raze RT: Duke Nukem 3D and Blood with path-traced lighting, for Apple silicon Macs.

The installer looks for your copies of the games on this Mac (and for extras you have downloaded, such as voxel models), then makes a launcher for each game.

macOS may ask whether the installer can read your Downloads folder: that is where it looks for the extras." "Quit" "Continue")"
[ "$r" = "Continue" ] || exit 0

notify "Looking for the games..."

# --- the games -----------------------------------------------------------------------

DUKE_DIR=""
BLOOD_DIR=""
# Duke: World Tour's folder first (its normal maps), then any DUKE3D.GRP
while IFS= read -r d; do
  [ -z "$DUKE_DIR" ] && DUKE_DIR="$d"
  if [ -d "$d/textures" ] || [ -n "$(find "$d" -maxdepth 2 -iname 'E5L1.MAP' 2>/dev/null | head -1)" ]; then DUKE_DIR="$d"; break; fi
done < <(dirs_with DUKE3D.GRP "${GAME_ROOTS[@]}")
while IFS= read -r d; do
  [ -z "$BLOOD_DIR" ] && BLOOD_DIR="$d"
  if [ -n "$(file_in "$d" 'SOUNDS.RFF')" ]; then BLOOD_DIR="$d"; break; fi
done < <(dirs_with BLOOD.RFF "${GAME_ROOTS[@]}")

DUKE_FOUND="Duke Nukem 3D  (found: ${DUKE_DIR/#$HOME/$TILDE})"
DUKE_LOCATE="Duke Nukem 3D  (I'll show you where its files are)"
BLOOD_FOUND="Blood  (found: ${BLOOD_DIR/#$HOME/$TILDE})"
BLOOD_LOCATE="Blood  (I'll show you where its files are)"
items=(); pre=()
if [ -n "$DUKE_DIR" ]; then items+=("$DUKE_FOUND"); pre+=("$DUKE_FOUND"); else items+=("$DUKE_LOCATE"); fi
if [ -n "$BLOOD_DIR" ]; then items+=("$BLOOD_FOUND"); pre+=("$BLOOD_FOUND"); else items+=("$BLOOD_LOCATE"); fi

chosen="$(pick "Which games do you want to install?

Games not found here can be located by hand (the folder with DUKE3D.GRP, or with BLOOD.RFF)." 1 "${pre[@]}" -- "${items[@]}")"
[ "$chosen" = "__quit__" ] && exit 0

WANT_DUKE=""; WANT_BLOOD=""
while IFS= read -r c; do
  case "$c" in
    "$DUKE_FOUND") WANT_DUKE=1 ;;
    "$DUKE_LOCATE")
      while :; do
        d="$(choose_folder "Choose the Duke Nukem 3D folder (the one with DUKE3D.GRP in it):")"
        [ -n "$d" ] || break
        d="${d%/}"
        if [ -n "$(file_in "$d" DUKE3D.GRP)" ]; then DUKE_DIR="$d"; WANT_DUKE=1; break; fi
        [ "$(message "There is no DUKE3D.GRP in that folder." "Skip Duke" "Choose again")" = "Choose again" ] || break
      done ;;
    "$BLOOD_FOUND") WANT_BLOOD=1 ;;
    "$BLOOD_LOCATE")
      while :; do
        d="$(choose_folder "Choose the Blood folder (the one with BLOOD.RFF in it):")"
        [ -n "$d" ] || break
        d="${d%/}"
        if [ -n "$(file_in "$d" BLOOD.RFF)" ]; then BLOOD_DIR="$d"; WANT_BLOOD=1; break; fi
        [ "$(message "There is no BLOOD.RFF in that folder." "Skip Blood" "Choose again")" = "Choose again" ] || break
      done ;;
  esac
done <<< "$chosen"
[ -n "$WANT_DUKE$WANT_BLOOD" ] || { message "No game was chosen: nothing installed." >/dev/null; exit 0; }

# --- the extras ----------------------------------------------------------------------

notify "Looking for extras (voxel models, HD textures, add-ons)..."

EXTRA_ROOTS=("$DOWNLOADS" "$HOME/Games")
# a folder with <file> under the roots, or an archive (.zip, .7z) in Downloads that holds
# <member>: the first found
find_extra() {
  local file="$1" member="$2" pattern="$3" d a
  d="$(dirs_with "$file" "${EXTRA_ROOTS[@]}" | grep -v '/Raze RT/' | head -1)"
  [ -n "$d" ] && { echo "dir:$d"; return; }
  for a in "$DOWNLOADS"/$pattern; do
    [ -f "$a" ] || continue
    if tar -tf "$a" 2>/dev/null | grep -qi "$member"; then echo "archive:$a"; return; fi
  done
}

extras=(); E_KEYS=()
add_extra() { extras+=("$1"); E_KEYS+=("$2"); }
if [ -n "$WANT_DUKE" ]; then
  DUKE_VOX="$(find_extra duke3d_voxels.def 'duke3d_voxels.def' '*[Vv]oxel*.zip')"
  [ -n "$DUKE_VOX" ] && add_extra "Duke: voxel models (Cheello's Voxel Duke Nukem 3D)" duke_vox
fi
if [ -n "$WANT_BLOOD" ]; then
  # Coagulated: its kvx/CHILLO folder
  BLOOD_VOX=""
  d="$(for r in "${EXTRA_ROOTS[@]}"; do [ -d "$r" ] && find "$r" -maxdepth 6 -type d -path '*/kvx/CHILLO' 2>/dev/null; done | grep -v '/Raze RT/' | head -1)"
  if [ -n "$d" ]; then BLOOD_VOX="dir:${d%/CHILLO}"
  else
    for a in "$DOWNLOADS"/[Cc]oagulated*.7z "$DOWNLOADS"/[Cc]oagulated*.zip; do
      [ -f "$a" ] && tar -tf "$a" 2>/dev/null | grep -q 'kvx/CHILLO/' && { BLOOD_VOX="archive:$a"; break; }
    done
  fi
  [ -n "$BLOOD_VOX" ] && add_extra "Blood: voxel models (Blood - Coagulated)" blood_vox
  BLOOD_HD="$(find_extra base-component.zip 'base-component.zip' '*[Uu]pscale*.zip')"
  [ -n "$BLOOD_HD" ] && add_extra "Blood: HD textures (Blood Upscale Pack)" blood_hd
  DW="$(dirs_with DWE1M1.MAP "${EXTRA_ROOTS[@]}" | grep -v '/Raze RT/' | head -1)"
  [ -n "$DW" ] && [ -z "$(file_in "$BLOOD_DIR/addons/$(basename "$DW")" DWE1M1.MAP)" ] && add_extra "Blood: Death Wish add-on" blood_dw
  MW="$(dirs_with MARROW.ini "${EXTRA_ROOTS[@]}" | grep -v '/Raze RT/' | head -1)"
  [ -n "$MW" ] && [ -z "$(file_in "$BLOOD_DIR/addons/$(basename "$MW")" MARROW.ini)" ] && add_extra "Blood: Marrow add-on" blood_mw
fi

TAKE=" "
if [ ${#extras[@]} -gt 0 ]; then
  sel="$(pick "These extras were found in your Downloads. Which do you want to install?

(Others can be added later: download them, then run the installer again. The Read Me has the links.)" 1 "${extras[@]}" -- "${extras[@]}")"
  [ "$sel" = "__quit__" ] && exit 0
  for i in "${!extras[@]}"; do
    grep -qxF "${extras[$i]}" <<< "$sel" && TAKE="$TAKE${E_KEYS[$i]} "
  done
fi
takes() { [[ "$TAKE" == *" $1 "* ]]; }

# --- install -------------------------------------------------------------------------

notify "Installing... (this can take a minute)"
mkdir -p "$DEST" || fail "Could not make $DEST."
exec 3>>"$LOG"

# extract <archive> into a new temporary folder: its path
extract() {
  local t; t="$(mktemp -d "${TMPDIR:-/tmp}/razert.XXXXXX")"
  tar -xf "$1" -C "$t" >&3 2>&1 || { rm -rf "$t"; return 1; }
  echo "$t"
}

# the engine (replaced whole: an update)
rm -rf "$DEST/Engine.new"
"$DITTO" "$PAYLOAD/Engine" "$DEST/Engine.new" >&3 2>&1 || fail "Could not copy the engine."
rm -rf "$DEST/Engine" && mv "$DEST/Engine.new" "$DEST/Engine"

# a game's folder: its settings and saves kept, the files that come with Raze RT updated
game_common() {
  local G="$1" T="$2" f
  mkdir -p "$G/user" "$G/logs" "$G/rt/mat_dev"
  # the path tracer's files: the engine's; the material table and its cache: the game's own
  for f in "$DEST/Engine/rt/"*; do ln -sfn "$f" "$G/rt/$(basename "$f")"; done
  cp -f "$T/rt/materials.txt" "$G/rt/materials.txt"
  cp -f "$T/defaults.ini" "$G/defaults.ini"
  [ -f "$G/user/raze.ini" ] || cp "$T/defaults.ini" "$G/user/raze.ini"
}

if [ -n "$WANT_DUKE" ]; then
  G="$DEST/Duke"; T="$PAYLOAD/Duke"
  game_common "$G" "$T"
  rm -rf "$G/overlay" "$G/overlay-raze"
  "$DITTO" "$T/overlay" "$G/overlay" >&3 2>&1
  "$DITTO" "$T/overlay-raze" "$G/overlay-raze" >&3 2>&1
  # World Tour's own normal maps (textures/TILESxxx/NNNN_n.bmp), as Duke-RT takes them
  if [ -d "$DUKE_DIR/textures" ]; then
    notify "Duke: taking World Tour's normal maps..."
    NM="$G/overlay/materials/normalmaps/auto"
    mkdir -p "$NM"
    while IFS= read -r -d '' f; do
      b="$(basename "$f" _n.bmp)"
      [[ "$b" =~ ^[0-9]+$ ]] || continue
      sips -s format png "$f" --out "$NM/$(printf '#%05d.png' "$((10#$b))")" >&3 2>&1
    done < <(find "$DUKE_DIR/textures" -name '*_n.bmp' -print0)
  fi
  if takes duke_vox; then
    notify "Duke: installing the voxel models..."
    src="${DUKE_VOX#*:}"; tmp=""
    if [ "${DUKE_VOX%%:*}" = archive ]; then
      tmp="$(extract "$src")" && src="$(dirname "$(find "$tmp" -name duke3d_voxels.def | head -1)")"
    fi
    if [ -f "$src/duke3d_voxels.def" ]; then
      rm -rf "$G/voxels"; "$DITTO" "$src" "$G/voxels" >&3 2>&1
    else
      echo "Duke voxels: no duke3d_voxels.def in $DUKE_VOX" >&3
    fi
    [ -n "$tmp" ] && rm -rf "$tmp"
  fi
  printf 'GAME_DIR=%q\n' "$DUKE_DIR" > "$G/game.conf"
fi

if [ -n "$WANT_BLOOD" ]; then
  G="$DEST/Blood"; T="$PAYLOAD/Blood"
  game_common "$G" "$T"
  mkdir -p "$G/hd" "$G/addons"
  rm -rf "$G/rt/windows"; "$DITTO" "$T/rt/windows" "$G/rt/windows" >&3 2>&1
  if takes blood_vox; then
    notify "Blood: installing the voxel models..."
    src="${BLOOD_VOX#*:}"; tmp=""
    if [ "${BLOOD_VOX%%:*}" = archive ]; then
      tmp="$(extract "$src")" && src="$(dirname "$(find "$tmp" -type d -path '*/kvx/CHILLO' | head -1)")"
    fi
    if [ -d "$src/CHILLO" ]; then
      rm -rf "$G/voxels"; mkdir -p "$G/voxels"
      "$DITTO" "$src" "$G/voxels/kvx" >&3 2>&1
      cp "$T/voxels/blood.def" "$T/voxels/blood-raze.def" "$G/voxels/"
    else
      echo "Blood voxels: no kvx/CHILLO in $BLOOD_VOX" >&3
    fi
    [ -n "$tmp" ] && rm -rf "$tmp"
  fi
  if takes blood_hd; then
    notify "Blood: installing the HD textures..."
    src="${BLOOD_HD#*:}"; tmp=""
    if [ "${BLOOD_HD%%:*}" = archive ]; then
      tmp="$(extract "$src")" && src="$(dirname "$(find "$tmp" -iname base-component.zip | head -1)")"
    fi
    # in this order: the textures, then SmoothBlood's weapon animations
    if [ -f "$src/base-component.zip" ]; then
      rm -f "$G/hd/"*.zip
      cp "$src/base-component.zip" "$G/hd/1-base-component.zip"
      [ -f "$src/smoothblood_upscale.zip" ] && cp "$src/smoothblood_upscale.zip" "$G/hd/2-smoothblood_upscale.zip"
    else
      echo "Blood HD: no base-component.zip in $BLOOD_HD" >&3
    fi
    [ -n "$tmp" ] && rm -rf "$tmp"
  fi
  if takes blood_dw; then
    rm -rf "$G/addons/Death Wish"; "$DITTO" "$DW" "$G/addons/Death Wish" >&3 2>&1
  fi
  if takes blood_mw; then
    rm -rf "$G/addons/Marrow"; "$DITTO" "$MW" "$G/addons/Marrow" >&3 2>&1
  fi
  printf 'GAME_DIR=%q\n' "$BLOOD_DIR" > "$G/game.conf"
fi

# --- the launchers -------------------------------------------------------------------

APPS="/Applications"
[ -w "$APPS" ] || { APPS="$HOME/Applications"; mkdir -p "$APPS"; }

make_launcher() {   # <game id> <app name> <icon>
  local A="$APPS/$2.app"
  rm -rf "$A"
  mkdir -p "$A/Contents/MacOS" "$A/Contents/Resources"
  cp "$PAYLOAD/launch.sh" "$A/Contents/MacOS/launch"
  chmod +x "$A/Contents/MacOS/launch"
  echo "$1" > "$A/Contents/Resources/game"
  cp "$PAYLOAD/$3" "$A/Contents/Resources/icon.icns"
  cat > "$A/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$2</string>
  <key>CFBundleDisplayName</key><string>$2</string>
  <key>CFBundleIdentifier</key><string>local.razert.play.$1</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$(cat "$PAYLOAD/VERSION" 2>/dev/null)</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
  touch "$A"
  echo "$A"
}

LAUNCHERS=()
[ -n "$WANT_DUKE" ] && LAUNCHERS+=("$(make_launcher duke "Duke Nukem 3D RT" duke.icns)")
[ -n "$WANT_BLOOD" ] && LAUNCHERS+=("$(make_launcher blood "Blood RT" blood.icns)")

# copied from a downloaded disk image: not to be held up by Gatekeeper again
xattr -dr com.apple.quarantine "$DEST" "${LAUNCHERS[@]}" >&3 2>&1

# --- done ----------------------------------------------------------------------------

names=""
for A in "${LAUNCHERS[@]}"; do names="$names
   $(basename "$A" .app)"; done
missing=""
[ -n "$WANT_DUKE" ] && [ ! -f "$DEST/Duke/voxels/duke3d_voxels.def" ] && missing="$missing
   Duke: voxel models"
if [ -n "$WANT_BLOOD" ]; then
  [ -d "$DEST/Blood/voxels/kvx" ] || missing="$missing
   Blood: voxel models"
  ls "$DEST/Blood/hd/"*.zip >/dev/null 2>&1 || missing="$missing
   Blood: HD textures"
fi
more=""
[ -n "$missing" ] && more="

Not installed (optional; the Read Me says where to get them, then run the installer again):$missing"

r="$(message "Done. The launchers are in your ${APPS/#$HOME/$TILDE} folder:$names

The first time each level is played, its materials are made and kept: it can look plain for a few seconds.$more" "Show in Finder" "OK")"
[ "$r" = "Show in Finder" ] && open -R "${LAUNCHERS[0]}"
exit 0
