#!/bin/bash
# Launch Raze with the RTGL1 path tracer. Duke Nukem 3D: World Tour by default;
# Blood: ~/Games/Blood-RT-Mac/run.sh (its own settings and material files)
#   cd ~/Games/Raze-RT-Mac && bash run.sh
#   bash run.sh --gl        the normal OpenGL renderer for this run
#   bash run.sh --rtdebug   RTGL1's info messages in the log too
#   bash run.sh --novoxels  without the voxel models (if setup.sh installed them)
#   bash run.sh --stats     frame time stats in logs/raze.log (slowest frame every 3 s,
#                           the GPU's timeline)
#   bash run.sh --emisdebug how each glowing texture glows and lights, once each, in the log
# Logs: logs/raze.log (game console) and logs/run.log.
cd "$(dirname "$0")"
ROOT="$(pwd)"
GAME="${DUKE_DIR:-$HOME/Games/Duke Nukem 3D World Tour}"
[ -d "$GAME" ] || GAME="$HOME/Downloads/Duke Nukem 3D 20th Anniversary World Tour"
DEPS="${RAZE_DEPS:-$HOME/Games/Duke-RT-Mac/deps}"
APP="$ROOT/build/raze.app/Contents/MacOS/raze"
[ -x "$APP" ] || { echo "No build yet - run: bash setup.sh"; exit 1; }
export DYLD_FALLBACK_LIBRARY_PATH="$DEPS/install/lib:/usr/local/lib:/usr/lib"
export RAZE_RT_DIR="$ROOT/rt"
mkdir -p logs user

EXTRA=()
PASS=()
NOVOX=
for a in "$@"; do
  case "$a" in
    --gl) export RAZE_NO_RT=1 ;;
    --rtdebug) EXTRA+=(-rtdebug) ;;
    --novoxels) NOVOX=1 ;;
    --stats) export RTGL1_METAL_PROFILE=1; EXTRA+=(+rt_debug_stats 1) ;;
    --emisdebug) EXTRA+=(+rt_debug_emis 1) ;;
    *) PASS+=("$a") ;;
  esac
done

if [ -z "${RAZE_NO_RT:-}" ] && [ ! -f "$RAZE_RT_DIR/bin/libRTGL1.dylib" ]; then
  echo "The path tracer's files are missing ($RAZE_RT_DIR/bin) - run: bash setup.sh"; exit 1
fi

ARGS=(-config "$ROOT/user/raze.ini" -savedir "$ROOT/user/saves" +logfile "$ROOT/logs/raze.log" +vid_fps 1 +vid_preferbackend 0)
if [[ " ${PASS[*]:-} " != *" -gamegrp "* ]]; then
  [ -f "$GAME/DUKE3D.GRP" ] || { echo "DUKE3D.GRP not found in: $GAME (set DUKE_DIR)"; exit 1; }
  ARGS=(-gamegrp "$GAME/DUKE3D.GRP" "${ARGS[@]}")
  # Duke-RT's lights (LIGHTOVR) and material maps -- Duke's tiles only
  [ -f "$ROOT/overlay/LIGHTOVR" ] && ARGS+=(-file "$ROOT/overlay")
  # Raze RT's own additions to them (setup.sh replaces overlay/ from Duke-RT's): glow maps
  # for lights Duke-RT left without one
  [ -d "$ROOT/overlay-raze" ] && ARGS+=(-file "$ROOT/overlay-raze")
  # Cheello's voxel models (setup.sh unpacks them)
  [ -z "$NOVOX" ] && [ -f "$ROOT/voxels/duke3d_voxels.def" ] && ARGS+=(-file "$ROOT/voxels")
fi
echo "$(date '+%Y-%m-%d %H:%M:%S') launching${RAZE_NO_RT:+ (OpenGL)}" > logs/run.log
"$APP" "${ARGS[@]}" "${EXTRA[@]}" "${PASS[@]}" >> logs/run.log 2>&1
rc=$?
[ $rc -ne 0 ] && echo "Game exited with code $rc - logs are in logs/"
exit $rc
