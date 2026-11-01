# Raze RT for macOS

Duke Nukem 3D and Blood with a real-time path tracer, running natively on Apple silicon Macs: Raze with the [RTGL1](https://github.com/sultim-t/RayTracedGL1) path tracer on Metal.

**Download:** the disk image is under [Releases](https://github.com/loui1989/Raze/releases). It holds an installer that finds your copies of the games, lets you pick which to install and makes a launcher for each. The Read Me on the disk image covers getting the game files, the optional extras and getting past Gatekeeper.

## What it does

- Path-traced lighting and shadows from every light the games have: lamps, fires, explosions, muzzle flashes, the sky and the moon through windows.
- Reflections on polished floors, mirrors and water; refracting glass, with coloured light through stained glass.
- Volumetric haze, smoke and ground fog that part as things move through them; heat haze over fires.
- Material maps (normal, roughness, metal, height) for the games' textures, made on first use and kept.
- Lights you can shoot out, with the glass's sound and the broken picture.
- Voxel models (Cheello's Voxel Duke Nukem 3D, Blood: Coagulated) and the Blood Upscale Pack, if you have them.
- MetalFX upscaling, dynamic resolution, and quality presets (Low, Medium, High) in Options > Ray Tracing.

Needs a Mac with Apple silicon (M3 or later recommended for its hardware ray tracing) and macOS 14 or newer, and your own copy of the games.

## Source

- This branch, `raze-rt-mac`: upstream Raze (commit `76fe82f5e`, 19 November 2025) with Raze RT's changes, one commit each. The renderer is in `source/common/rendering/rt/`.
- The path tracer: the `raze-rt-mac` branch of [loui1989/RayTracedGL1](https://github.com/loui1989/RayTracedGL1/tree/raze-rt-mac), RTGL1 with a Metal backend and Raze RT's shader changes.
- `mac/raze/`: the build (`setup.sh`), the game launchers, the disk image and its installer (`package/`), Duke's material table and Raze RT's additions to Duke-RT's lights (`overlay-raze/`).
- `mac/blood/`: Blood's launcher, material table, window glass masks and the voxel definitions for Blood: Coagulated.

The scripts expect the layout they were made in: `~/Games/Raze-RT-Mac` (the engine, from `mac/raze`) and `~/Games/Blood-RT-Mac` (from `mac/blood`). `setup.sh` takes upstream Raze and applies the changes as patches; the commits on this branch are those patches.

## Credits

Raze: the ZDoom team, after EDuke32, NBlood and Build (Ken Silverman). RTGL1: Sultim Tsyrendashiev. Duke-RT's lights and material maps: [postmemetic/Duke-RT](https://github.com/postmemetic/Duke-RT). Voxel models: Cheello and others. Blood Upscale Pack: Phredreeke and others.

Raze RT is free software under the GNU GPL, version 2 or later, like Raze. Duke Nukem 3D and Blood are not included.
