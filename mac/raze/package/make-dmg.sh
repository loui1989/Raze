#!/bin/bash
# Makes the shareable disk image of Raze RT (Duke Nukem 3D and Blood):
#   cd ~/Games/Raze-RT-Mac && bash package/make-dmg.sh
# -> dist/Raze-RT-<date>.dmg, with "Install Raze RT", the Read Me, and the licences and
# source (the patches). Run it on this Mac after a build (the games' launchers build it).
#
# What goes in: the engine as built here (its libraries moved into it, so it runs on any
# Apple silicon Mac), the path tracer, Duke-RT's lights and material maps, Raze RT's own
# files (material tables, Blood's window masks, the voxel definitions for Coagulated)
# and the settings as tuned here (each game's raze.ini, its ray tracing part only).
# What does not: the games, and other people's extras (voxel models, HD textures,
# add-ons) -- the installer takes those from the player's own downloads.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
PKG="$ROOT/package"
BLOOD="${BLOOD_RT:-$HOME/Games/Blood-RT-Mac}"
DEPS="${RAZE_DEPS:-$HOME/Games/Duke-RT-Mac/deps}/install/lib"
VERSION="$(date +%Y.%m.%d)"
say() { printf '  %s\n' "$*"; }

[ -x build/raze.app/Contents/MacOS/raze ] || { echo "No build here yet (start a game once, or bash setup.sh)"; exit 1; }
[ -f "$BLOOD/rt/materials.txt" ] || { echo "Blood RT's folder not found ($BLOOD; set BLOOD_RT)"; exit 1; }
for t in otool install_name_tool codesign hdiutil ditto; do
  command -v "$t" >/dev/null || { echo "$t is missing (Xcode's command line tools: xcode-select --install)"; exit 1; }
done

STAGE="$(mktemp -d /tmp/razert-dmg.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT
VOL="$STAGE/Raze RT"
INST="$VOL/Install Raze RT.app"
P="$INST/Contents/Resources/Payload"
mkdir -p "$INST/Contents/MacOS" "$P"

# --- the engine, with its libraries inside it -----------------------------------------
say "Engine"
ditto build/raze.app "$P/Engine/raze.app"
FW="$P/Engine/raze.app/Contents/Frameworks"
mkdir -p "$FW"

# bundle <library>: a library from deps/ into Frameworks/, named @rpath/<library>, and the
# deps/ libraries it uses with it
bundle() {
  local name="$1" dep dn
  [ -f "$FW/$name" ] && return 0
  [ -f "$DEPS/$name" ] || { echo "   (no $name in $DEPS)"; return 0; }
  cp -L "$DEPS/$name" "$FW/$name"
  chmod u+w "$FW/$name"
  install_name_tool -id "@rpath/$name" "$FW/$name" 2>/dev/null
  for dep in $(otool -L "$FW/$name" | tail -n +2 | awk '{print $1}'); do
    case "$dep" in
      "$DEPS"/*|@rpath/*)
        dn="$(basename "$dep")"
        [ "$dn" = "$name" ] && continue
        install_name_tool -change "$dep" "@rpath/$dn" "$FW/$name" 2>/dev/null
        bundle "$dn"
        ;;
    esac
  done
  install_name_tool -add_rpath "@loader_path" "$FW/$name" 2>/dev/null || true
}
# linked: ZMusic; loaded by name at run time: OpenAL (sound), libsndfile (ZMusic's codecs)
for lib in libzmusiclite.1.dylib libopenal.1.dylib libsndfile.1.dylib; do bundle "$lib"; done

R="$P/Engine/raze.app/Contents/MacOS/raze"
for rp in $(otool -l "$R" | awk '/cmd LC_RPATH/{getline; getline; print $2}'); do
  case "$rp" in /Users/*|/opt/*|/usr/local/*) install_name_tool -delete_rpath "$rp" "$R" ;; esac
done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$R"
# anything still pointing into this Mac's folders would not load elsewhere
if otool -L "$R" "$FW"/*.dylib | grep -q "$HOME"; then
  otool -L "$R" "$FW"/*.dylib | grep "$HOME"
  echo "Libraries still point into $HOME (above)"; exit 1
fi
say "  libraries: $(cd "$FW" && ls | tr '\n' ' ')"

# the path tracer (and its shaders), and the files it reads
mkdir -p "$P/Engine/rt"
ditto rt/bin "$P/Engine/rt/bin"
cp rt/*.ktx2 rt/RTGL1.json "$P/Engine/rt/"

# ad hoc signatures (the libraries were changed; Apple silicon runs nothing unsigned)
# (the program itself, as the build signs it: not the bundle -- its game data next to the
# program, raze.pk3 and the soundfonts, are not code)
codesign --force -s - "$FW"/*.dylib "$P/Engine/rt/bin/libRTGL1.dylib"
# (signed outside the bundle: codesign would take the bundle's other files in MacOS/ for
# code and refuse; the signature is in the program itself either way)
cp "$R" "$STAGE/raze.bin"
codesign --force -s - "$STAGE/raze.bin"
cp "$STAGE/raze.bin" "$R"
rm -f "$STAGE/raze.bin"

# --- each game's files ------------------------------------------------------------------

# seed <raze.ini>: the ray tracing settings as tuned here, for a new player's raze.ini
seed() {
  echo "[GlobalSettings]"
  awk '/^\[/{ s = ($0 == "[GlobalSettings]") } s && /^rt_/ && !/^rt_debug/' "$1"
  echo "vid_raytracing=true"
  echo "vid_fullscreen=true"
  echo "vid_rt_fullscreen_height=0"
  echo
  echo "[LastRun]"
  awk '/^\[/{ s = ($0 == "[LastRun]") } s && /^Version=/' "$1"
}

say "Duke Nukem 3D"
D="$P/Duke"
mkdir -p "$D/rt"
cp rt/materials.txt "$D/rt/"
# (not World Tour's own normal maps: the installer takes them from the player's copy)
rsync -a --exclude 'materials/normalmaps/auto' overlay/ "$D/overlay/"
rsync -a overlay-raze/ "$D/overlay-raze/"
seed user/raze.ini > "$D/defaults.ini"

say "Blood"
B="$P/Blood"
mkdir -p "$B/rt" "$B/voxels"
cp "$BLOOD/rt/materials.txt" "$B/rt/"
ditto "$BLOOD/rt/windows" "$B/rt/windows"
cp "$BLOOD/voxels/blood.def" "$BLOOD/voxels/blood-raze.def" "$B/voxels/"
seed "$BLOOD/user/raze.ini" > "$B/defaults.ini"

# --- the installer --------------------------------------------------------------------
say "Installer"
cp "$PKG/installer/install.sh" "$INST/Contents/MacOS/install"
cp "$PKG/installer/launch.sh" "$P/launch.sh"
chmod +x "$INST/Contents/MacOS/install" "$P/launch.sh"
cp "$PKG/icons/duke.icns" "$PKG/icons/blood.icns" "$P/"
cp "$PKG/icons/installer.icns" "$INST/Contents/Resources/icon.icns"
echo "$VERSION" > "$P/VERSION"
cat > "$INST/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Install Raze RT</string>
  <key>CFBundleIdentifier</key><string>local.razert.installer</string>
  <key>CFBundleExecutable</key><string>install</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
codesign --force -s - "$INST" 2>/dev/null || true

# --- the Read Me, licences and source ------------------------------------------------------
cp "$PKG/Read Me.html" "$VOL/Read Me.html"
L="$VOL/Licences and Source"
mkdir -p "$L/Source"
cp "$PKG/CREDITS.txt" "$L/"
# Raze's (GPL 2, and the Build licence for its Build code) and RTGL1's (MIT)
for f in src/package/common/gpl-2.0.txt src/package/common/buildlic.txt; do
  if [ -f "$f" ]; then cp "$f" "$L/"; fi
done
cp "$PKG/RTGL1-LICENSE.txt" "$L/"
# the source: Raze and RTGL1 as built here are their upstream versions with these
# patches (setup.sh: which versions, and how they are applied)
ditto patches "$L/Source/patches"
cp setup.sh "$L/Source/setup.sh"

# --- the disk image -------------------------------------------------------------------
mkdir -p dist
DMG="dist/Raze-RT-$VERSION.dmg"
say "Disk image"
hdiutil create -volname "Raze RT" -srcfolder "$VOL" -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$DMG" >/dev/null
say "Done: $ROOT/$DMG ($(du -h "$DMG" | cut -f1))"
open -R "$DMG"
