# Project guidance

## Do NOT run commands — write them for the user

**Only WRITE the commands that need to be run; the user runs them.** Do not execute
build / install / device / shell commands yourself — output them as text (in a code
block) and let the user run them. This covers Gradle, CMake, `adb`, and shell
commands in general. After making code changes, describe what changed, give any
commands to run, and stop. Do not build/run "to verify."

Exception: parsing/reading data files the user explicitly hands over for analysis
(e.g. a CSV export they point you to) is fine — that's the analysis they asked for.

## Reference implementation: maplibre-gl-js

Take **`/mnt/shared/nkt-app-test/maplibre-gl-js`** as the canonical reference for
rendering behavior — especially 3D terrain. When porting or fixing terrain in this
(native) repo, first check how maplibre-gl-js does it and match that behavior.

Why: maplibre-gl-js is the mature, correct implementation. The native terrain
pipeline (this branch's WIP) should mirror its architecture rather than invent new
approaches. Key areas to cross-check against gl-js:

- **Drape / render-to-texture caching** — gl-js renders each draped tile texture
  and reuses it across frames; it does not repaint the whole style into every tile
  every frame. This is the main terrain performance lever (measured on device:
  re-rendering all drape targets every frame produces ~30x pixel overdraw and pins
  the GPU fragment core at ~95%, ~1.4 FPS).
- **Terrain surface pass** — depth mode, blending, cull order, and how the drape
  texture is sampled onto the DEM-displaced mesh.
- **DEM decoding / elevation sampling** — encoding (terrarium vs Terrain-RGB),
  vertex texture fetch, tile fallback while a tile's DEM loads.
- **Coordinate / matrix conventions** — tile space, Y-flip, projection matrices.

Relevant gl-js source: `src/render/`, `src/terrain/` (e.g. terrain drape, render
cache), and the terrain-related render passes.
