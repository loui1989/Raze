#!/bin/bash
# Raze RT launcher: plays Duke Nukem 3D or Blood -- and when the patches have changed
# since the last build (a new update was dropped into patches/), builds it first, then
# starts the game. The build runs in the app itself (so macOS lets it read the Doom 64
# RT files in Downloads: the app is what asks for that); a Terminal window only shows
# its progress.
#   launcher.sh duke | blood      what the apps (Duke Nukem 3D RT, Blood RT) run
ENGINE="$HOME/Games/Raze-RT-Mac"
BLOOD="$HOME/Games/Blood-RT-Mac"
cd "$ENGINE" || exit 1
mkdir -p "$ENGINE/build" "$ENGINE/logs"
STAMP="$ENGINE/build/.launcher-stamp"     # the patches the last good build was made from
LOCK="$ENGINE/build/.launcher-updating"   # the PID of a build in progress
LOG="$ENGINE/logs/update.log"

current_stamp() {
  { cat setup.sh patches/raze/series patches/raze/*.patch patches/rtgl1/*.patch patches/rtgl1-lib/*.patch 2>/dev/null; } | shasum | cut -c1-40
}
game_dir() { [ "$1" = blood ] && echo "$BLOOD" || echo "$ENGINE"; }
alert() {
  /usr/bin/osascript -e "display dialog \"$1\" with title \"Raze RT\" buttons {\"OK\"} default button 1 with icon caution" >/dev/null 2>&1
}

GAMEID="${1:-duke}"

# Blood: what to play -- the game, an add-on episode, or a game carried on from another
# port (user/loadouts/*.txt, named by their first line). Asked first; remembered.
RUN_ARGS=()
if [ "$GAMEID" = blood ]; then
  PICKS=("Blood")
  for a in "Cryptic Passage" "Death Wish" "Marrow"; do
    for d in "$BLOOD/addons/$a" "$BLOOD/blood-files/addons/$a"; do
      [ -d "$d" ] && { PICKS+=("$a"); break; }
    done
  done
  LOADOUTS=()
  for f in "$BLOOD"/user/loadouts/*.txt; do
    [ -f "$f" ] || continue
    t="$(head -1 "$f" | sed 's/^# *//; s/ *--.*$//; s/"/'"'"'/g')"
    PICKS+=("$t"); LOADOUTS+=("$t|$(basename "$f" .txt)")
  done
  LAST="$(cat "$BLOOD/user/last-pick" 2>/dev/null)"
  list=""; found=
  for p in "${PICKS[@]}"; do list="$list, \"$p\""; [ "$p" = "$LAST" ] && found=1; done
  [ -n "$found" ] || LAST="Blood"
  PICK="$(/usr/bin/osascript 2>/dev/null <<OSA
activate
set c to choose from list {${list#, }} with title "Blood RT" with prompt "What do you want to play?" default items {"$LAST"} OK button name "Play" cancel button name "Quit"
if c is false then return ""
return item 1 of c
OSA
)"
  [ -n "$PICK" ] || exit 0
  echo "$PICK" > "$BLOOD/user/last-pick"
  case "$PICK" in
    Blood) ;;
    "Cryptic Passage") RUN_ARGS=(--cryptic) ;;
    "Death Wish") RUN_ARGS=(--deathwish) ;;
    Marrow) RUN_ARGS=(--marrow) ;;
    *) for l in "${LOADOUTS[@]}"; do [ "${l%%|*}" = "$PICK" ] && RUN_ARGS=(--loadout "${l#*|}"); done ;;
  esac
fi
if [ -f "$LOCK" ] && kill -0 "$(cat "$LOCK")" 2>/dev/null; then
  alert "An update is being built already (its progress is in a Terminal window). The game starts when it is done."
  exit 0
fi
rm -f "$LOCK"

if [ ! -x "$ENGINE/build/raze.app/Contents/MacOS/raze" ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$(current_stamp)" ]; then
  echo $$ > "$LOCK"
  trap 'rm -f "$LOCK"' EXIT
  printf '\n  Raze RT: there is a new update -- building it first (a few minutes).\n\n' > "$LOG"
  # the progress, in Terminal (it only reads the log)
  /usr/bin/osascript >/dev/null 2>&1 <<OSA
tell application "Terminal"
  activate
  do script "clear; tail -n +1 -f '$LOG'"
end tell
OSA
  if bash setup.sh >> "$LOG" 2>&1; then
    current_stamp > "$STAMP"
    rm -f "$LOCK"
    printf '\n  Done. Starting the game -- you can close this window.\n' >> "$LOG"
    cd "$(game_dir "$GAMEID")" || exit 1
    exec bash run.sh "${RUN_ARGS[@]}"
  fi
  rm -f "$LOCK"
  if grep -q "Operation not permitted" "$LOG"; then
    printf '\n  macOS did not let the app read your Downloads folder (the Doom 64 RT files are\n  there). Allow it: System Settings > Privacy & Security > Files and Folders >\n  %s > Downloads Folder (on). Then open the game again.\n' "${RAZE_RT_APP:-the game app}" >> "$LOG"
  fi
  printf '\n  The update did not build. The details are above, and in %s/logs/\n  (update.log, build-errors.txt). You can close this window.\n' "$ENGINE" >> "$LOG"
  alert "The update did not build -- the Terminal window shows why."
  exit 1
fi
cd "$(game_dir "$GAMEID")" || exit 1
exec bash run.sh "${RUN_ARGS[@]}"
