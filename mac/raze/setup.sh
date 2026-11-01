#!/bin/bash
# Raze RT for Mac — upstream Raze (Duke Nukem 3D, Blood, Shadow Warrior, Exhumed, Redneck Rampage) on macOS,
# the base for the RTGL1 path tracer.
#
#   cd ~/Games/Raze-RT-Mac && bash setup.sh
#
# Upstream Raze plus patches/raze, with the RTGL1 path tracer from Doom 64 RT for macOS
# as its renderer. Safe to re-run.
# The libraries (ZMusic, OpenAL Soft, libvpx, audio codecs) and the cmake/ninja tools are taken from
# ~/Games/Duke-RT-Mac/deps, which are already built there.
# The path tracer: RTGL1's headers from the Doom 64 RT source tree, its built library and Metal
# shaders from the Doom 64 RT build (copied into rt/bin), its data files from the Doom 64 RT data
# folder (copied into rt/).

set -u
cd "$(dirname "$0")"
ROOT="$(pwd)"
mkdir -p logs
STATUS="$ROOT/logs/status.txt"
: > "$STATUS"
say()  { echo "==> $*"; echo "$(date '+%H:%M:%S') $*" >> "$STATUS"; }
fail() { echo "FAILED: $*"; echo "$(date '+%H:%M:%S') FAILED: $*" >> "$STATUS"; exit 1; }

RAZE_REPO="https://github.com/ZDoom/Raze.git"
RAZE_COMMIT="76fe82f5e"                      # upstream master, 19 Nov 2025
DEPS="${RAZE_DEPS:-$HOME/Games/Duke-RT-Mac/deps}"
JOBS="$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || sysctl -n hw.ncpu)"
D64="${DOOM64RT_DIR:-$HOME/Downloads/doom64-rt-mac}"
# rtgl1-kit: the parts of the Doom 64 RT build this needs (RTGL1's headers and shader
# sources, its library and shaders, glslang and SpirvToMsl), kept here -- macOS does
# not let the game's apps read Downloads. Without it, they come from $D64.
KIT="$ROOT/rtgl1-kit"
if [ -d "$KIT/RayTracedGL1/Include" ]; then
  RTGL1_SDK="${RTGL1_SDK_PATH:-$KIT/RayTracedGL1}"
  RTGL1_BUILD="${RTGL1_BUILD:-$KIT/out/metal}"
  KIT_GLSLANG="$KIT/bin/glslang"
  KIT_SPIRV2MSL="$KIT/bin/SpirvToMsl"
else
  RTGL1_SDK="${RTGL1_SDK_PATH:-$D64/src/RayTracedGL1}"
  RTGL1_BUILD="${RTGL1_BUILD:-$D64/build/out/metal}"
  KIT_GLSLANG="$D64/build/glslang/StandAlone/glslang"
  KIT_SPIRV2MSL="$D64/build/rtgl1-metal/SpirvToMsl"
fi
D64DATA="${DOOM64RT_DATA:-$HOME/Games/Doom 64 RT}"
DUKERT_OVERLAY="${DUKERT_OVERLAY:-$HOME/Games/Duke-RT-Mac/src-rt/release-overlay}"
DUKE_GAME="${DUKE_DIR:-$HOME/Games/Duke Nukem 3D World Tour}"   # (a copy of the Downloads one: see rtgl1-kit)
[ -d "$DUKE_GAME" ] || DUKE_GAME="$HOME/Downloads/Duke Nukem 3D 20th Anniversary World Tour"

say "Checking tools and libraries"
xcode-select -p >/dev/null 2>&1 || fail "Xcode command-line tools missing. Run: xcode-select --install"
[ -x "$DEPS/tools/cmake/CMake.app/Contents/bin/cmake" ] && [ -x "$DEPS/tools/bin/ninja" ] \
  || fail "cmake/ninja not found in $DEPS/tools (run Duke-RT-Mac's setup.sh once, or set RAZE_DEPS)"
ls "$DEPS"/install/lib/libzmusiclite* >/dev/null 2>&1 || fail "ZMusic not found in $DEPS/install"
export PATH="$DEPS/tools/cmake/CMake.app/Contents/bin:$DEPS/tools/bin:$PATH"
[ -f "$RTGL1_SDK/Include/RTGL1/RTGL1.h" ] || fail "RTGL1 headers not found in $RTGL1_SDK (set RTGL1_SDK_PATH)"

if [ ! -d src/.git ]; then
  say "Downloading Raze source (a few minutes)"
  git clone --filter=blob:none --no-checkout "$RAZE_REPO" src > logs/clone.log 2>&1 || fail "git clone Raze (see logs/clone.log)"
fi
say "Preparing src (Raze $RAZE_COMMIT + patches/raze)"
{
  git -C src checkout -q -f "$RAZE_COMMIT" &&
  git -C src clean -fdq &&
  while read -r patch; do
    [ -n "$patch" ] && git -C src apply --whitespace=nowarn "$ROOT/patches/raze/$patch" || exit 1
  done < patches/raze/series
} > logs/src.log 2>&1 || fail "preparing src (see logs/src.log)"

say "Configuring Raze"
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_PREFIX_PATH="$DEPS/install" -DRTGL1_SDK_PATH="$RTGL1_SDK" > logs/configure.log 2>&1 || fail "CMake configure (see logs/configure.log)"
grep -q "Ray tracing renderer: RTGL1" logs/configure.log || fail "CMake did not pick up RTGL1 (see logs/configure.log)"

say "Building Raze (first build takes several minutes)"
start=$(date +%s)
ninja -C build -j "$JOBS" -k 0 > logs/build.log 2>&1
rc=$?
secs=$(( $(date +%s) - start ))
grep -E "error:|FAILED:" logs/build.log | head -200 > logs/build-errors.txt
[ $rc -eq 0 ] || fail "Build failed after ${secs}s — $(grep -c 'error:' logs/build.log) errors (see logs/build-errors.txt)"
say "Build OK in ${secs}s: build/raze.app"

# the path tracer's run time files: rt/bin (library + Metal shaders) and rt/ (data)
say "Updating rt/ (the path tracer's files)"
mkdir -p rt/bin
# a library is put as a new file (copy, then rename over the old one): macOS keeps a loaded
# library's code signature by file, and one rewritten in place is killed when it loads
put_lib() { cp -f "$1" rt/bin/.libRTGL1.dylib.new && mv -f rt/bin/.libRTGL1.dylib.new rt/bin/libRTGL1.dylib; }
if [ -f "$RTGL1_BUILD/libRTGL1.dylib" ]; then
  put_lib "$RTGL1_BUILD/libRTGL1.dylib" || fail "copying libRTGL1.dylib"
  for f in RTGL1.metallib RTGL1.metal; do
    [ -f "$RTGL1_BUILD/shaders/$f" ] && cp -f "$RTGL1_BUILD/shaders/$f" "rt/bin/$f"
  done
  [ -d "$RTGL1_BUILD/shaders/shaders" ] && rsync -a --delete "$RTGL1_BUILD/shaders/shaders/" rt/bin/shaders/
elif [ -f "$D64DATA/rt/bin/libRTGL1.dylib" ]; then
  rsync -a --delete "$D64DATA/rt/bin/" rt/bin/
else
  fail "libRTGL1.dylib not found in $RTGL1_BUILD or $D64DATA/rt/bin (build Doom 64 RT first)"
fi
[ -f rt/bin/RTGL1.metallib ] || [ -f rt/bin/RTGL1.metal ] || fail "RTGL1's Metal shaders (RTGL1.metallib or RTGL1.metal) not found"

# RTGL1 itself with Raze RT's changes (patches/rtgl1-lib/*.patch, its Metal backend): built
# from a copy of the Doom 64 RT build's source (rtgl1-kit/src), in place of the library above.
# Made again when a patch changes; if it does not build, Doom 64 RT's library is used as it is.
if ls patches/rtgl1-lib/*.patch >/dev/null 2>&1 && [ -d "$KIT/src/RayTracedGL1/Source" ]; then
  RL="$ROOT/build/rtgl1-lib"
  stamp="$( { cat patches/rtgl1-lib/*.patch; ls -lR "$KIT/src/RayTracedGL1/Source/Metal"; } | shasum | cut -c1-40 )"
  if [ "$(cat "$RL/stamp" 2>/dev/null)" != "$stamp" ] || [ ! -f "$RL/libRTGL1.dylib" ]; then
    say "Building the path tracer library with Raze RT's changes (the first time takes a few minutes)"
    mkdir -p "$RL"
    rm -f "$RL/libRTGL1.dylib" "$RL/stamp"
    : > logs/rtgl1-lib.log
    ok=0
    if rsync -a --delete --exclude /Build "$KIT/src/RayTracedGL1/" "$RL/src/" >> logs/rtgl1-lib.log 2>&1; then
      ok=1
      for p in patches/rtgl1-lib/*.patch; do
        patch -s -p1 -d "$RL/src" < "$p" >> logs/rtgl1-lib.log 2>&1 || { ok=0; echo "patch $p did not apply" >> logs/rtgl1-lib.log; break; }
      done
    fi
    if [ $ok = 1 ] &&
       cmake -S "$RL/src" -B "$RL/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DRG_WITH_METAL=ON -DRG_WITH_EXAMPLES=OFF \
         -DRG_SPIRV_CROSS_DIR="$KIT/src/SPIRV-Cross" -DRG_GLSLANG="$KIT_GLSLANG" >> logs/rtgl1-lib.log 2>&1 &&
       ninja -C "$RL/build" -j "$JOBS" RayTracedGL1 >> logs/rtgl1-lib.log 2>&1 &&
       [ -f "$RL/src/Build/bin/libRTGL1.dylib" ] && cp -f "$RL/src/Build/bin/libRTGL1.dylib" "$RL/libRTGL1.dylib"; then
      echo "$stamp" > "$RL/stamp"
      say "Path tracer library OK"
    else
      echo "   (the path tracer library did not build - see logs/rtgl1-lib.log; Doom 64 RT's is used as it is)"
      echo "$(date '+%H:%M:%S') path tracer library did not build (logs/rtgl1-lib.log)" >> "$STATUS"
    fi
  fi
  [ -f "$RL/libRTGL1.dylib" ] && put_lib "$RL/libRTGL1.dylib"
fi
# Raze RT's changes to the path tracer's shaders (patches/rtgl1/*.patch): RTGL1's GLSL
# from the Doom 64 RT source tree with them applied, compiled with that build's own
# tools (glslang, SpirvToMsl) and put over rt/bin/shaders. Made again when a patch or
# the shader source changes; if they don't compile, Doom 64 RT's are used as they are.
if ls patches/rtgl1/*.patch >/dev/null 2>&1; then
  RS="$ROOT/build/rtgl1-shaders"
  GLSLANG_BIN="${RTGL1_GLSLANG:-$KIT_GLSLANG}"
  SPIRV2MSL="${RTGL1_SPIRV_TO_MSL:-$KIT_SPIRV2MSL}"
  stamp="$( { cat patches/rtgl1/*.patch; ls -lR "$RTGL1_SDK/Source/Shaders" "$RTGL1_SDK/Source/Generated"; } | shasum | cut -c1-40 )"
  if [ "$(cat "$RS/stamp" 2>/dev/null)" != "$stamp" ] || [ ! -d "$RS/out" ]; then
    if [ -x "$GLSLANG_BIN" ] && [ -x "$SPIRV2MSL" ]; then
      say "Compiling the path tracer's shaders with Raze RT's changes (a minute or two)"
      rm -rf "$RS" && mkdir -p "$RS/src/Source/Metal"
      cp -R "$RTGL1_SDK/Source/Shaders" "$RTGL1_SDK/Source/Generated" "$RS/src/Source/"
      cp "$RTGL1_SDK/Source/Metal/CompileShadersMetal.py" "$RTGL1_SDK/Source/Metal/MslIntersector.py" "$RS/src/Source/Metal/"
      ok=1
      for p in patches/rtgl1/*.patch; do
        patch -s -p1 -d "$RS/src" < "$p" > logs/shaders.log 2>&1 || { ok=0; echo "   (patch $(basename "$p") did not apply - see logs/shaders.log)"; break; }
      done
      if [ $ok = 1 ] && python3 "$RS/src/Source/Metal/CompileShadersMetal.py" --shaders "$RS/src/Source/Shaders" \
           --generated "$RS/src/Source/Generated" --build "$RS/glsl" --output "$RS/out" \
           --spirv-to-msl "$SPIRV2MSL" --glslang "$GLSLANG_BIN" --msl-only >> logs/shaders.log 2>&1; then
        echo "$stamp" > "$RS/stamp"
        say "Shaders OK: $(ls "$RS/out" | grep -c '\.metal$') files"
      else
        rm -rf "$RS/out"
        echo "   (the shaders did not compile - see logs/shaders.log; Doom 64 RT's are used as they are)"
      fi
    else
      echo "   (glslang or SpirvToMsl not found in the Doom 64 RT build - its shaders are used as they are)"
    fi
  fi
  [ -d "$RS/out" ] && rsync -a "$RS/out/" rt/bin/shaders/
fi
for f in BlueNoise_LDR_RGBA_128.ktx2 WaterNormal_n.ktx2 DirtMask.ktx2 SceneBuildWarning.ktx2; do
  if [ -f "$D64DATA/rt/$f" ]; then cp -f "$D64DATA/rt/$f" "rt/$f"
  elif [ -f "$RTGL1_SDK/Tools/$f" ]; then cp -f "$RTGL1_SDK/Tools/$f" "rt/$f"
  fi
done
[ -f rt/BlueNoise_LDR_RGBA_128.ktx2 ] || fail "BlueNoise_LDR_RGBA_128.ktx2 not found (in $D64DATA/rt or $RTGL1_SDK/Tools)"
[ -f rt/RTGL1.json ] || cat > rt/RTGL1.json <<'JSON'
{
  "version": 0,
  "developerMode": true,
  "debugWindows": false,
  "vulkanValidation": false,
  "dx12Validation": false,
  "dlssValidation": false,
  "fpsMonitor": false
}
JSON
# Duke-RT's authored lights (LIGHTOVR) and material maps, mounted by run.sh with -file
if [ -f "$DUKERT_OVERLAY/LIGHTOVR" ]; then
  say "Updating overlay/ (Duke-RT's lights and material maps)"
  mkdir -p overlay
  cp -f "$DUKERT_OVERLAY/LIGHTOVR" overlay/LIGHTOVR
  for d in glowmaps roughness metallic specular; do
    [ -d "$DUKERT_OVERLAY/materials/$d" ] && mkdir -p "overlay/materials/$d" && rsync -a --delete "$DUKERT_OVERLAY/materials/$d/" "overlay/materials/$d/"
  done
else
  echo "   (Duke-RT's overlay not found in $DUKERT_OVERLAY - no authored lights or material maps)"
fi
# World Tour's normal maps (textures/TILESxxx/NNNN_n.bmp), as Duke-RT's launcher imports them
if [ -d "$DUKE_GAME/textures" ]; then
  NM="overlay/materials/normalmaps/auto"
  mkdir -p "$NM"
  n=0
  while IFS= read -r -d '' f; do
    b="$(basename "$f" _n.bmp)"
    [[ "$b" =~ ^[0-9]+$ ]] || continue
    out="$NM/$(printf '#%05d.png' "$((10#$b))")"
    if [ ! -f "$out" ] || [ "$f" -nt "$out" ]; then
      sips -s format png "$f" --out "$out" >/dev/null 2>&1 && n=$((n+1))
    fi
  done < <(find "$DUKE_GAME/textures" -name '*_n.bmp' -print0)
  say "Normal maps from World Tour: $n new, $(ls "$NM" | wc -l | tr -d ' ') in all"
fi
# Cheello's Voxel Duke 3D (3D models for the sprites), if its zip is in Downloads:
#   https://www.moddb.com/mods/voxel-duke-nukem-3d/addons/voxel-duke-3d
VOXZIP="${VOXEL_ZIP:-$(ls -t "$HOME/Downloads"/voxel_duke3d*.zip "$HOME/Downloads"/*[Vv]oxel*[Dd]uke*.zip 2>/dev/null | head -1)}"
if [ ! -f voxels/duke3d_voxels.def ] && [ -n "$VOXZIP" ] && [ -f "$VOXZIP" ]; then
  say "Unpacking the voxel models from $(basename "$VOXZIP")"
  rm -rf logs/voxtmp && mkdir -p logs/voxtmp
  if unzip -q "$VOXZIP" -d logs/voxtmp; then
    vdef="$(find logs/voxtmp -name duke3d_voxels.def | head -1)"
    if [ -n "$vdef" ]; then
      mkdir -p voxels && cp -R "$(dirname "$vdef")"/. voxels/
    else
      echo "   (no duke3d_voxels.def in that zip - not the voxel pack?)"
    fi
  fi
  rm -rf logs/voxtmp
fi
[ -f voxels/duke3d_voxels.def ] && say "Voxel models: on (run.sh --novoxels to play without)"
say "Ready: bash run.sh"
