#!/bin/bash
# Blood with the RTGL1 path tracer. The engine is the Raze build of Raze-RT-Mac
# (one Raze plays both games); this folder keeps Blood's own settings, saves, logs
# and material files apart from Duke's -- and none of Duke-RT's overlay or voxels.
#   cd ~/Games/Blood-RT-Mac && bash run.sh
#   bash run.sh --gl        the normal OpenGL renderer for this run
#   bash run.sh --rtdebug   RTGL1's info messages in the log too
#   bash run.sh --stats     frame time stats in logs/raze.log (slowest frame every 3 s,
#                           the GPU's timeline)
#   bash run.sh --novoxels  without the voxel models (Blood - Coagulated, in voxels/)
#   bash run.sh --nohd      without the HD textures (Blood Upscale Pack 2.1, in hd/)
#   bash run.sh --deathwish   the Death Wish add-on episodes (also --marrow, --cryptic,
#   bash run.sh --addon "Name"  or any add-on folder with its .ini: in addons/,
#                           blood-files/addons/ or the game's addons/); its own saves
#   bash run.sh --loadout marrow-e1m8   carry on a game from another port: a level
#                           started with the things in user/loadouts/<name>.txt
#                           (its "addon:" line picks the add-on)
# Blood's files: Steam's by default, or BLOOD_DIR=/path/to/Blood bash run.sh
# Logs: logs/raze.log (game console) and logs/run.log.
cd "$(dirname "$0")"
ROOT="$(pwd)"
ENGINE="${RAZE_RT_MAC:-$HOME/Games/Raze-RT-Mac}"
GAME="${BLOOD_DIR:-$HOME/Library/Application Support/Steam/SteamApps/common/Blood}"
DEPS="${RAZE_DEPS:-$HOME/Games/Duke-RT-Mac/deps}"
APP="$ENGINE/build/raze.app/Contents/MacOS/raze"
[ -x "$APP" ] || { echo "No Raze build in $ENGINE - run: bash setup.sh there first"; exit 1; }
[ -f "$GAME/BLOOD.RFF" ] || { echo "BLOOD.RFF not found in: $GAME (set BLOOD_DIR)"; exit 1; }
export DYLD_FALLBACK_LIBRARY_PATH="$DEPS/install/lib:/usr/local/lib:/usr/lib"
mkdir -p logs user rt/mat_dev

# the path tracer's files: the engine's, but Blood's own material files (mat_dev):
# they are named by tile number, and Blood's tile 126 is not Duke's
for f in "$ENGINE"/rt/*; do
  n="$(basename "$f")"
  [ "$n" = "mat_dev" ] && continue
  [ -e "rt/$n" ] || ln -s "$f" "rt/$n"
done
export RAZE_RT_DIR="$ROOT/rt"

# first run: start from the Duke port's settings (keys, mouse, RT options)
[ -f user/raze.ini ] || { [ -f "$ENGINE/user/raze.ini" ] && cp "$ENGINE/user/raze.ini" user/raze.ini; }

EXTRA=()
PASS=()
NOVOX=
NOHD=
ADDON=
LOADOUT=
for a in "$@"; do
  case "$a" in
    --gl) export RAZE_NO_RT=1 ;;
    --rtdebug) EXTRA+=(-rtdebug) ;;
    --stats) export RTGL1_METAL_PROFILE=1; EXTRA+=(+rt_debug_stats 1) ;;
    --novoxels) NOVOX=1 ;;
    --nohd) NOHD=1 ;;
    --deathwish|--dw) ADDON="Death Wish" ;;
    --marrow) ADDON="Marrow" ;;
    --cryptic|--cp) ADDON="Cryptic Passage" ;;
    --addon=*) ADDON="${a#--addon=}" ;;
    --addon) ADDON="__next__" ;;
    --loadout=*) LOADOUT="${a#--loadout=}" ;;
    --loadout) LOADOUT="__next__" ;;
    *) [ "$ADDON" = "__next__" ] && { ADDON="$a"; continue; }
       [ "$LOADOUT" = "__next__" ] && { LOADOUT="$a"; continue; }
       PASS+=("$a") ;;
  esac
done

if [ -z "${RAZE_NO_RT:-}" ] && [ ! -f "$RAZE_RT_DIR/bin/libRTGL1.dylib" ]; then
  echo "The path tracer's files are missing ($ENGINE/rt/bin) - run: bash setup.sh in $ENGINE"; exit 1
fi

# a loadout file: its add-on, and the loadout command (lines without '#' or 'addon:')
LOADOUT_CMD=()
if [ -n "$LOADOUT" ]; then
  LF="$ROOT/user/loadouts/${LOADOUT%.txt}.txt"
  [ -f "$LF" ] || { echo "No loadout file: $LF"; exit 1; }
  la="$(grep -i '^addon:' "$LF" | head -1 | sed 's/^[Aa][Dd][Dd][Oo][Nn]: *//; s/ *$//')"
  [ -n "$la" ] && ADDON="$la"
  LOADOUT_CMD=(+loadout $(grep -v -i '^ *#\|^addon:' "$LF"))
fi

# an add-on: its folder (maps, art, music, cutscenes) and its episode list (.ini)
ADDON_DIR=
ADDON_INI=
SAVES="$ROOT/user/saves"
if [ -n "$ADDON" ]; then
  for d in "$ROOT/addons/$ADDON" "$ROOT/blood-files/addons/$ADDON" "$GAME/addons/$ADDON"; do
    [ -d "$d" ] && { ADDON_DIR="$d"; break; }
  done
  [ -n "$ADDON_DIR" ] || { echo "Add-on not found: $ADDON (looked in addons/, blood-files/addons/ and $GAME/addons/)"; exit 1; }
  ADDON_INI="$(cd "$ADDON_DIR" && ls | grep -i '\.ini$' | head -1)"
  [ -n "$ADDON_INI" ] || { echo "No .ini (episode list) in $ADDON_DIR"; exit 1; }
  SAVES="$ROOT/user/saves-$(echo "$ADDON" | tr -cd '[:alnum:]' | tr '[:upper:]' '[:lower:]')"
fi

# (-nosetup: no game picker at the start -- the game is given here)
ARGS=(-nosetup -gamegrp "$GAME/BLOOD.RFF" -config "$ROOT/user/raze.ini" -savedir "$SAVES" +logfile "$ROOT/logs/raze.log" +vid_fps 1 +vid_preferbackend 0)
# Phredreeke's HD textures and sprites, with SmoothBlood's weapon animations (Blood
# Upscale Pack 2.1: its zips, in name order) -- before the voxels: a voxel model wins
if [ -z "$NOHD" ]; then
  for z in "$ROOT"/hd/*.zip; do [ -f "$z" ] && ARGS+=(-file "$z"); done
fi
# Cheello's voxel monsters and props (Blood - Coagulated)
[ -z "$NOVOX" ] && [ -f "$ROOT/voxels/blood.def" ] && ARGS+=(-file "$ROOT/voxels")
# the add-on last: its art and files win
[ -n "$ADDON_DIR" ] && ARGS+=(-file "$ADDON_DIR" -ini "$ADDON_INI")
echo "$(date '+%Y-%m-%d %H:%M:%S') launching${RAZE_NO_RT:+ (OpenGL)}${ADDON:+ - $ADDON ($ADDON_INI)}" > logs/run.log
"$APP" "${ARGS[@]}" "${EXTRA[@]}" "${PASS[@]}" "${LOADOUT_CMD[@]}" >> logs/run.log 2>&1
rc=$?
[ $rc -ne 0 ] && echo "Game exited with code $rc - logs are in logs/"
exit $rc
