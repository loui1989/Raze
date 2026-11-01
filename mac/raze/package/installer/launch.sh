#!/bin/bash
# Raze RT game launcher, made by the installer: "Duke Nukem 3D RT.app" and "Blood RT.app"
# run this, with the game (duke or blood) in Contents/Resources/game.
#
# Everything is in ~/Library/Application Support/Raze RT: the engine (Engine/) and each
# game's own folder (Duke/, Blood/) -- its settings and saves (user/), logs (logs/), its
# material table and the materials made from its textures (rt/), and its extras (voxel
# models, HD textures, add-ons). The game's own files stay where they are (game.conf).

RES="$(cd "$(dirname "$0")/../Resources" && pwd)"
GAMEID="$(cat "$RES/game" 2>/dev/null)"
BASE="$HOME/Library/Application Support/Raze RT"
ENGINE="$BASE/Engine"
OSASCRIPT="${OSASCRIPT:-/usr/bin/osascript}"
case "$GAMEID" in
  blood) G="$BASE/Blood"; TITLE="Blood RT" ;;
  *)     GAMEID=duke; G="$BASE/Duke"; TITLE="Duke Nukem 3D RT" ;;
esac

alert() {
  "$OSASCRIPT" - "$TITLE" "$1" >/dev/null 2>&1 <<'OSA'
on run argv
  activate
  display dialog (item 2 of argv) with title (item 1 of argv) buttons {"OK"} default button 1 with icon caution
end run
OSA
}

APP="$ENGINE/raze.app/Contents/MacOS/raze"
if [ ! -x "$APP" ] || [ ! -f "$G/game.conf" ]; then
  alert "$TITLE is not installed completely. Run \"Install Raze RT\" again."
  exit 1
fi
GAME_DIR=""
. "$G/game.conf"
cd "$G" || exit 1
mkdir -p logs user rt/mat_dev
[ -f user/raze.ini ] || cp defaults.ini user/raze.ini

# the engine's libraries (OpenAL, ZMusic's codecs) and the path tracer
export DYLD_FALLBACK_LIBRARY_PATH="$ENGINE/raze.app/Contents/Frameworks:/usr/local/lib:/usr/lib"
export RAZE_RT_DIR="$G/rt"

ARGS=(-config "$G/user/raze.ini" +logfile "$G/logs/raze.log" +vid_preferbackend 0)

if [ "$GAMEID" = duke ]; then
  GRP="$(find "$GAME_DIR" -maxdepth 1 -type f -iname DUKE3D.GRP 2>/dev/null | head -1)"
  if [ -z "$GRP" ]; then
    alert "Duke Nukem 3D's files are not where they were ($GAME_DIR: no DUKE3D.GRP). Run \"Install Raze RT\" again to find them."
    exit 1
  fi
  ARGS=(-gamegrp "$GRP" "${ARGS[@]}" -savedir "$G/user/saves")
  # Duke-RT's lights and material maps, Raze RT's additions, and the voxel models
  [ -f overlay/LIGHTOVR ] && ARGS+=(-file "$G/overlay")
  [ -d overlay-raze ] && ARGS+=(-file "$G/overlay-raze")
  [ -f voxels/duke3d_voxels.def ] && ARGS+=(-file "$G/voxels")
else
  RFF="$(find "$GAME_DIR" -maxdepth 1 -type f -iname BLOOD.RFF 2>/dev/null | head -1)"
  if [ -z "$RFF" ]; then
    alert "Blood's files are not where they were ($GAME_DIR: no BLOOD.RFF). Run \"Install Raze RT\" again to find them."
    exit 1
  fi
  # what to play: Blood, or an add-on episode (a folder with its episode list, an .ini:
  # Raze RT's addons/, or the game's own -- Fresh Supply's Cryptic Passage)
  PICKS=("Blood")
  ADDON_DIRS=()
  for d in "$G/addons"/*/ "$GAME_DIR/addons"/*/ "$GAME_DIR/ADDONS"/*/; do
    [ -d "$d" ] || continue
    d="${d%/}"
    [ -n "$(find "$d" -maxdepth 1 -type f -iname '*.ini' | head -1)" ] || continue
    n="$(basename "$d")"
    dup=""; for p in "${PICKS[@]}"; do [ "$p" = "$n" ] && dup=1; done
    [ -n "$dup" ] && continue
    PICKS+=("$n"); ADDON_DIRS+=("$d")
  done
  ADDON_DIR=""
  if [ ${#PICKS[@]} -gt 1 ]; then
    LAST="$(cat user/last-pick 2>/dev/null)"
    found=""; for p in "${PICKS[@]}"; do [ "$p" = "$LAST" ] && found=1; done
    [ -n "$found" ] || LAST="Blood"
    PICK="$("$OSASCRIPT" - "$LAST" "${PICKS[@]}" 2>/dev/null <<'OSA'
on run argv
  activate
  set c to choose from list (items 2 thru -1 of argv) with title "Blood RT" with prompt "What do you want to play?" default items {item 1 of argv} OK button name "Play" cancel button name "Quit"
  if c is false then return ""
  return item 1 of c
end run
OSA
)"
    [ -n "$PICK" ] || exit 0
    echo "$PICK" > user/last-pick
    for i in "${!ADDON_DIRS[@]}"; do
      [ "${PICKS[$((i + 1))]}" = "$PICK" ] && ADDON_DIR="${ADDON_DIRS[$i]}"
    done
  fi
  SAVES="$G/user/saves"
  ARGS=(-nosetup -gamegrp "$RFF" "${ARGS[@]}")
  # the HD textures (in name order: textures, then weapon animations), then the voxel
  # models (a voxel model wins over a picture), then the add-on (its files win)
  for z in "$G"/hd/*.zip; do [ -f "$z" ] && ARGS+=(-file "$z"); done
  [ -f voxels/blood.def ] && ARGS+=(-file "$G/voxels")
  if [ -n "$ADDON_DIR" ]; then
    INI="$(cd "$ADDON_DIR" && ls | grep -i '\.ini$' | head -1)"
    ARGS+=(-file "$ADDON_DIR" -ini "$INI")
    SAVES="$G/user/saves-$(basename "$ADDON_DIR" | tr -cd '[:alnum:]' | tr '[:upper:]' '[:lower:]')"
  fi
  ARGS+=(-savedir "$SAVES")
fi

echo "$(date '+%Y-%m-%d %H:%M:%S') launching $TITLE${ADDON_DIR:+ - $(basename "$ADDON_DIR")}" > logs/run.log
"$APP" "${ARGS[@]}" >> logs/run.log 2>&1
rc=$?
if [ $rc -ne 0 ] && [ $rc -ne 130 ] && [ $rc -ne 143 ]; then
  alert "$TITLE closed with an error ($rc). What happened is in:
$G/logs/raze.log and run.log"
fi
exit $rc
