# HD / Modern Graphics Remaster — Engineering Roadmap

This document describes how Omnispeak could be extended to render at a higher
resolution with higher-fidelity ("HD") art, while keeping the original game
logic, physics, and demo compatibility intact. It is a design and planning
document, not a finished implementation.

It is written against the renderer as it exists today and references the
relevant source locations so the work can be picked up incrementally.

## 1. What "upscaling" actually means here

Omnispeak is a *pixel-perfect reimplementation* of Commander Keen 4–6. It does
not ship its own art — it reads the original DOS data files (`EGAGRAPH.CKx`,
`EGAHEAD.CKx`, `EGADICT.CKx`, with layout in `GFXINFOE.CKx`). Those files store
**16-colour EGA planar graphics** designed for a **320×200** playfield. See
`doc/modding.md` §Graphics.

So there are three distinct things people mean by "upscaling", in increasing
order of effort:

1. **Output scaling** — show the same 320×200, 16-colour frame larger and with
   nicer filtering. *Already implemented* (see §2).
2. **Real-time upscaling filters** — run the existing low-res frame through an
   edge-aware shader (xBRZ / HQx / ScaleFX / CRT). No new art. A self-contained
   change to the GL backend.
3. **True HD art** — replace the low-res sprites/tiles/bitmaps with
   higher-resolution, higher-colour-depth artwork. This is the subject of this
   document. It is a content project as much as a code project: the engine
   change is bounded, but someone has to *draw* (or AI-upscale and clean up)
   every asset.

The critical constraint, established by reading the renderer, is below.

## 2. Current rendering pipeline (as built today)

```
game logic (ck_*.c)
  └─ RF refresh manager (src/id_rf.c)       — "Virtual Tile Refresh", 16×16 tiles
       └─ id_ca.c                            — loads & Huffman-decodes EGAGRAPH chunks
       └─ id_vh.c / id_vl.c                   — EGA-planar → 8-bit palette-index blits
            └─ rf_tileBuffer + screen surface — ONE 320×200, 8-bit PAL8 buffer
                 └─ VL_Backend->present()     — uploads PAL8 as a texture, scales to window
```

Key facts, with sources:

- **Native size and depth.** `VL_EGAVGA_GFX_WIDTH 320`, `VL_EGAVGA_GFX_HEIGHT
  200` (`src/id_vl_private.h:27`). Every surface the game composites into is
  8-bit palette-indexed; the 16 EGA colours live in `VL_EGARGBColorTable`
  (`src/id_vl.c`).
- **Everything is composited at native res before scaling.** Tiles are blitted
  into `rf_tileBuffer` (336×224, a 21×14-tile buffer +border) by
  `RF_RenderTile16` / `RF_RenderTile16m` (`src/id_rf.c:658`/`:672`), the visible
  region is copied to the screen surface, and sprites are drawn over the top by
  `RFL_DrawSpriteList` → `VH_DrawShiftedSprite` (`src/id_rf.c:1348`).
- **The backend only ever sees the finished low-res, palette-indexed frame.**
  The SDL2-GL backend uploads it as a single 8-bit texture and a fragment shader
  does the palette lookup, then scales to the window:
  `gl_FragColor = texture1D(palette, texture2D(screenBuf, ...).r)`
  (`src/id_vl_sdl2gl.c`, ~line 210, in `VL_SDL2GL_Present`,
  `src/id_vl_sdl2gl.c:561`). The SDL2 software backend does the same conversion
  on the CPU.
- **Coordinate system.** The game works in *units* (1 tile = 256 units = 16
  pixels). Macros `RF_UnitToPixel`, `RF_TileToPixel`, etc. live in
  `src/id_rf.h:64`. Sprites carry an `originX/originY` and a sub-pixel `shift`
  (0–3, representing the 0/2/4/6 px EGA fine-scroll positions) — see
  `RF_AddSpriteDraw` (`src/id_rf.c:1235`) and the `shift` handling at `:1300`.
- **Scaling/aspect already exist.** Integer scaling, 4:3 aspect correction, and
  overscan border are handled in `VL_CalculateRenderRegions`
  (`src/id_vl.c`) and exposed via `/INTEGER`, `/FILLED`, `/NOBORDER`.

**Consequence:** the renderer is a hard 320×200, 16-colour bottleneck. You can
display that frame at 4K, but the *information* is still 320×200×16-colour.
Achieving true HD requires drawing the HD assets into a **separate
high-resolution layer that bypasses the PAL8 buffer**, composited in the backend
at output resolution. The game logic continues to run unchanged at native res
and tells us *where* each tile/sprite is; the HD layer decides *how* it looks.

## 3. Design principles (non-negotiables)

1. **Logic stays at native resolution.** Collision, physics, AI, scroll limits,
   and demo playback must be byte-for-byte identical. We only change rendering.
   This keeps demo compatibility and avoids reworking thousands of lines of
   gameplay code.
2. **Graceful fallback per-asset.** If an HD replacement for a given chunk does
   not exist, fall back to the upscaled EGA original. This lets the art be
   produced incrementally instead of all-or-nothing.
3. **Original data files untouched.** HD art ships as a *separate* asset pack
   (a new directory / archive), selected at runtime. Do not modify the
   `EGAGRAPH` pipeline. This respects the modding model in `doc/modding.md` and
   keeps the EGA path as the source of truth for positions and timing.
4. **Backend-isolated.** The HD compositor is implemented behind the existing
   `VL_Backend` abstraction (`src/id_vl.h:91`). The CPU/DOS backends keep
   working in EGA mode and simply do not advertise HD support.

## 4. Proposed architecture

### 4.1 HD asset pack

An asset pack is gated by the config key `vl_hdAssets` (parsed like `rf_minTics`
in `src/id_rf.c:614`) or the `/HD` command-line switch, both handled in
`VL_Startup` (`src/id_vl.c`).

The manifest is a flat text file (no JSON dependency) that maps **original
chunk numbers** to image files plus metadata. As implemented (`src/id_vl_hd.c`,
`VL_HD_ParseLine`) each non-blank, non-`#` line is:

```
# chunk   file              scale  originX  originY
1234      bg_1234.bmp        4
5678      keen_walk0.bmp     4      40       28
```

- `chunk` is the **absolute** graphics chunk number (tiles, sprites and bitmaps
  share one chunk-number space, so a single chunk→image map suffices). Only
  `chunk` and `file` are required.
- `scale` is the HD-pixels-per-EGA-pixel factor (e.g. 4 → a 16×16 tile becomes
  64×64); defaults to 1.
- For sprites, `originX/originY` are the EGA-pixel hotspot, derived from the EGA
  sprite's `originX/originY` (`VH_GetSpriteTableEntry`, used in
  `src/id_rf.c:1298`) so the HD art lands at the correct game position.

**File locations.** The case-insensitive file opener
(`FSL_OpenFileInDirCaseInsensitive`, `src/id_fs.c:80`) matches a single
directory entry, so HD files currently live *flat* in the Keen data path
(opened via `FS_OpenKeenFile`), next to `EGAGRAPH.CKx`. The manifest filename
defaults to `omnispeak_hd.txt` (override with the `vl_hdManifest` config key).
A future improvement is a dedicated HD search path so packs can live in their
own subdirectory.

**Image format.** Phase 1 decodes **32-bit BMP** via SDL's built-in
`SDL_LoadBMP_RW`, which keeps the engine dependency-free. PNG support can be
added later behind the same `loadImage` backend hook (e.g. via optional
SDL_image or a vendored decoder).

Chunk numbers are stable and already symbolically named in `GFXCHUNK.CKx`
(`doc/modding.md:122`), which makes building the manifest tractable — a tool can
dump every chunk to an image (idGrab / a `--dumpgfx`-style exporter) to use as a
redraw/upscale base.

### 4.2 HD backend interface

`VL_Backend` (`src/id_vl.h`) carries an optional `VL_HDBackend *hd` pointer,
kept separate from the EGA blit functions. It is the **last** field of
`VL_Backend`, so the positional initializers in the non-HD backends (which omit
it) zero-fill it to `NULL` — they need no changes and advertise no HD support.

The interface as shipped in Phase 1 covers asset loading only:

```c
typedef struct VL_HDBackend {
    bool  (*hasHD)(void);
    void *(*loadImage)(const void *fileData, int dataLen, int *outW, int *outH);
    void  (*destroyImage)(void *image);
} VL_HDBackend;
```

The backend-agnostic asset manager (`src/id_vl_hd.c`) reads the manifest, pulls
each image file's bytes through `FS_OpenKeenFile`, hands them to
`hd->loadImage`, and stores the resulting opaque handles in a chunk-sorted table
queried via `VL_HD_GetImage(chunk)`. The SDL2-GL implementation
(`VL_SDL2GL_HD_LoadImage`) decodes the BMP and uploads an RGBA GL texture.

The **compositor draw path** (begin-frame / draw-quad / end-frame, rendering HD
quads into an output-resolution FBO — reusing the framebuffer machinery at
`src/id_vl_sdl2gl.c`, `vl_sdl2gl_framebufferTexture` /
`id_glFramebufferTexture2DEXT`) is intentionally **deferred to Phase 2**, where
it is delivered together with the refresh-manager hooks that exercise it (§4.3),
so it can actually be tested rather than landing as untested dead code.

### 4.3 Hooking the refresh manager

The cleanest insertion point is to mirror the existing draw calls. Each place
that draws an EGA tile or sprite gets an "also record an HD draw" sibling, gated
by `vl_hdAssets`:

- **Tiles.** `RF_RenderTile16` / `RF_RenderTile16m` (`src/id_rf.c:658`,`:672`)
  already know `(x, y, tile)`. When HD is on and an HD replacement exists for
  that tile chunk, record an HD quad at the tile's buffer position instead of
  (or in addition to, for fallback) the PAL8 blit. Because tiles are
  static per buffer cell, they can also be drawn directly from the map each
  frame in the HD path rather than going through the dirty-block buffer —
  simpler, and modern GPUs do not need the Virtual Tile Refresh optimisation.
- **Sprites.** `RFL_DrawSpriteList` (`src/id_rf.c:1348`) walks the z-ordered
  sprite table and calls `VH_DrawShiftedSprite(pixelX, pixelY, chunk, shift)`.
  This is the ideal hook: it already has final screen-pixel position, chunk
  number, z-layer ordering, and sub-pixel `shift`. Add an HD branch that calls
  `drawHDQuad` with the HD texture for `chunk`. The `maskOnly` path
  (`VH_DrawShiftedSpriteMask`, used for the "all white" damage flash at
  `src/id_rf.c:1392`) maps to `drawHDQuad(..., maskOnly=true, tintColor)`.
- **Foreground tiles** drawn over sprites (`RFL_RenderForeTiles`,
  `src/id_rf.c:781`) need the same treatment so z-ordering (foreground over
  sprites over background) is preserved in the HD layer.

When HD mode is active, the HD FBO becomes the thing presented to the window;
the PAL8 path can either be skipped for replaced assets or kept underneath as
the fallback for not-yet-replaced ones. The simplest correct approach for
incremental art: **render the full EGA frame upscaled as the base layer, then
draw HD quads on top only for chunks that have HD replacements.** This
guarantees a complete picture at every stage of the art project.

### 4.4 Sub-pixel scrolling and shifts

The EGA engine fine-scrolls in 2px steps and pre-shifts sprites into 4 phases
(`shift` 0–3, `src/id_rf.c:1301`). In the HD layer we have real sub-pixel
positioning, so `shift` collapses to a fractional offset: `offsetPx = shift *
2 * egaToScreenScale / 1`. The HD art is drawn once per chunk (no pre-shifted
variants needed), and smooth scrolling comes for free from positioning the
quads at `scrollXpx` granularity finer than the EGA `& 0xef` mask applied at
`src/id_rf.c:1450`. (Keep feeding the EGA path its masked scroll value; give the
HD path the unmasked value for genuinely smooth motion.)

## 5. Phasing

The work is sequenced so each phase is independently shippable and testable.

**Phase 0 — Shader upscaling (low risk, immediate payoff).**
Add an edge-aware upscale shader pass to `id_vl_sdl2gl.c` between the palette
lookup and the final blit. No asset work, no logic changes. This is the
highest value-per-effort step and de-risks the FBO/shader plumbing the later
phases reuse. Expose via config (`vl_filter = none|crt|xbr`).

**Phase 1 — HD plumbing, no art.**
Implement `VL_HDBackend` in the GL backend, the FBO compositor, the manifest
loader, and the config/CLI switch. Ship with an empty manifest: behaviour is
identical to today (everything falls back to upscaled EGA). Verifiable by
confirming zero visual change with `/HD` and an empty pack.

**Phase 2 — Tiles.**
Hook `RF_RenderTile16`/`RF_RenderTile16m`/`RFL_RenderForeTiles`. Produce HD
tilesets for one level as a proof of concept. Background art is the easiest to
replace and gives the biggest visual lift.

**Phase 3 — Sprites.**
Hook `RFL_DrawSpriteList`. Handle z-layering, the mask/tint flash path, and
origin/hotspot mapping. Replace Keen + common enemies first.

**Phase 4 — UI / bitmaps / fonts.**
The menu, status bar, and fonts go through `id_vh.c` bitmap/`VWB` text paths;
give them the same chunk-replacement treatment.

**Phase 5 — Tooling & docs.**
A `--dumpgfx` exporter (chunk → PNG) to seed the art, a manifest validator, and
a `doc/modding.md` section documenting the HD pack format so the community can
contribute art packs the same way they do EGA mods.

## 6. Asset production strategy

The engine work above is on the order of a few thousand lines. The *art* is the
real cost: Keen 4–6 contain thousands of tiles, sprite frames, and bitmaps.
Realistic options, usually combined:

- **AI super-resolution + manual cleanup** (ESRGAN-class upscalers tuned for
  pixel art) to bootstrap, then hand-fix outlines and palettes.
- **Hand-redrawn art** for hero assets (Keen, key enemies, UI) where quality
  matters most.
- **Community art packs**, distributed separately from the GPL engine and from
  the copyrighted original data, selected at runtime — mirroring the existing
  mod ecosystem.

Because of the per-asset fallback (§3.2), the project is shippable at every
point: any chunk without HD art simply renders upscaled-EGA.

## 7. Risks and open questions

- **Demo/recording compatibility.** Must be preserved. Mitigated by keeping all
  logic and the EGA timing path (`RFL_CalcTics`, `src/id_rf.c:986`) untouched;
  HD is render-only.
- **DOS/CPU backends.** They cannot do HD; they advertise `hasHD()==false` and
  run EGA as today. No regression.
- **Memory/VRAM.** HD textures are far larger than EGA chunks. Need a texture
  cache keyed on chunk number with LRU eviction, paralleling `id_ca.c`'s chunk
  caching but for GPU resources.
- **Palette effects.** Vanilla Keen animates the palette (fades, the border
  colour, light-switch levels). HD art has baked colours, so palette fades must
  be re-expressed as a shader uniform (a brightness/tint multiplier applied to
  HD quads) driven by the same `vl_palette` fade state (`src/id_vl.h:39`).
- **Mixed-fidelity look.** During incremental art production, HD assets sit next
  to upscaled-EGA ones. Acceptable for development; a "complete pack" is the
  release bar.

## 8. Phase 0: shipped — output filters

The SDL2+OpenGL backend now supports a configurable output-pass filter,
applied during the final FBO→window step in `VL_SDL2GL_Present`
(`src/id_vl_sdl2gl.c`). Selected via the config key `vl_filter`:

- `none` (default) — unchanged. Uses `glBlitFramebufferEXT` fast path when
  available.
- `scanlines` — alternating dim/bright horizontal lines tracking the rendered
  EGA scanline grid.
- `crt` — scanlines plus an RGB sub-pixel mask and a soft vignette.

When a filter is selected, the FBO→window pass goes through a textured-quad
draw with a small fragment shader instead of the framebuffer blit. If a
filter's shader fails to compile or link at startup, the backend logs a
warning and silently falls back to `none`, so no configuration can render
the game unplayable.

This is a first, deliberately simple iteration; higher-quality upscalers
(xBR, ScaleFX, Lanczos) can be added later as additional `vl_filter` values
behind the same plumbing.

## 9. Phase 1: shipped — HD asset plumbing

The foundational, render-inert half of the HD path is now in place:

- **Backend interface.** `VL_HDBackend` (`src/id_vl.h`) with
  `hasHD`/`loadImage`/`destroyImage`, hung off `VL_Backend` as an optional,
  trailing `hd` pointer so the other backends zero-fill it to `NULL` and need
  no changes.
- **Asset manager.** `src/id_vl_hd.c` / `.h` — reads the manifest, loads each
  image's bytes through `FS_OpenKeenFile`, uploads them via the backend, and
  exposes `VL_HD_GetImage(chunk)` (chunk-sorted table + `bsearch`).
- **GL implementation.** `VL_SDL2GL_HD_LoadImage` decodes 32-bit BMP with
  `SDL_LoadBMP_RW` and uploads an RGBA texture; `VL_SDL2GL_HD_DestroyImage`
  frees it.
- **Switches.** `/HD` command-line flag and the `vl_hdAssets` config key (plus
  `vl_hdManifest` for the manifest filename). Wired through `VL_Startup` /
  `VL_Shutdown`, calling `VL_HD_Startup` / `VL_HD_Shutdown`.

**It is inert by design.** With HD disabled (the default), or no manifest
present, or on a backend without HD support, behaviour is byte-for-byte the
EGA build. When enabled with a manifest, assets load (and log a count) but
nothing is *drawn* yet — the compositor draw path and the refresh-manager
hooks that feed it are Phase 2/3. This keeps Phase 1 fully buildable and
testable on its own: success criteria are "no change when off" and "manifest
loads without crashing when on".

Failure modes degrade gracefully: a missing manifest, an unreadable image, a
non-HD backend, or a decode failure each log a warning and fall back to EGA.

## 10. Phase 2: shipped — HD tile compositor

The HD draw path is now wired up for tiles:

- **Backend interface.** `VL_HDBackend` gained `beginFrame` /
  `drawQuad(image, bufferPxX, bufferPxY, egaW, egaH)` / `endFrame`
  (`src/id_vl.h`). Coordinates are in **EGA buffer pixels** — the same
  coordinate space as `VL_SetScrollCoords` — so the existing scroll plumbing
  applies unchanged to HD draws.
- **Manager wrappers.** `VL_HD_BeginFrame` / `VL_HD_DrawChunk(chunk, ...)` /
  `VL_HD_EndFrame` (`src/id_vl_hd.c`). `DrawChunk` looks up the chunk and
  silently no-ops if there's no HD replacement, so callers can submit
  blindly for every cell.
- **GL implementation.** `VL_SDL2GL_HD_FlushFrame` plays back the recorded
  list as textured quads, drawn into the same FBO as the EGA quad, with the
  same viewport (`vl_renderRgn`). NDC math maps each buffer-pixel
  rectangle to the visible region using the same `scrlX/scrlY` Present
  already receives. Uses fixed-function texturing (no second shader) with
  `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` blending so HD foreground tiles
  composite correctly over HD or EGA backgrounds.
- **Refresh-manager hook.** `RF_Refresh` (`src/id_rf.c`) walks all 21 × 14
  visible buffer cells once per frame and submits `VL_HD_DrawChunk` for
  the bg plane (`ca_gfxInfoE.offTiles16 + bgTile`) and, if non-zero, the fg
  plane (`ca_gfxInfoE.offTiles16m + fgTile`). EGA blits to `rf_tileBuffer`
  continue unchanged, so cells without an HD replacement fall back to
  upscaled EGA pixel-for-pixel. No dirty-block tracking on the HD path —
  modern GPUs don't need it, and the chunk-replacement model is unaffected
  by which code site originally called `RF_RenderTile16`.

**Fallback granularity is per cell, per plane.** A pack can replace just one
or two tiles and everything else continues to render as EGA.

**Known limitation.** If a cell's bg has an HD replacement but its fg does
not (and the EGA fg is non-empty), the HD bg covers the EGA fg, since HD
quads composite over the upscaled EGA frame. Pack authors should provide
HD fg for any cell where they replace bg AND the EGA fg is non-empty. This
is a documentation contract, not a code limit.

Per-frame cost: at most 21·14·2 = 588 chunk lookups + bounded GPU draws (a
fraction of one millisecond on any GPU shipped this century). Off-screen
cells produce quads that fall outside the viewport and are clipped by GL.

## 11. Phase 3: shipped — HD sprites & z-order

Sprites now get the same chunk-replacement treatment, composited in correct
z-order with the HD tiles from Phase 2.

- **Draw order.** The whole HD frame is now built inside `RFL_DrawSpriteList`
  (`src/id_rf.c`), mirroring the EGA draw order exactly:
  1. `RFL_SubmitHDBackgroundTiles` — bg plane for every cell, plus
     **non-fore** fg-plane tiles (`!(TI_ForeMisc(tile) & 0x80)`).
  2. Sprite z-layers 0–2.
  3. `RFL_SubmitHDForeTiles` — **fore**-flagged fg-plane tiles, on top of
     those sprites (submitted right after the existing `RFL_RenderForeTiles`).
  4. Sprite z-layer 3.
  This replaces Phase 2's single end-of-frame tile loop (which couldn't
  interleave sprites). `RF_Refresh` just wraps `RFL_DrawSpriteList` with
  `VL_HD_BeginFrame` / `VL_HD_EndFrame`.
- **Sprite placement.** Each in-bounds sprite is submitted every frame
  (no dirty gating — the GPU redraws the lot) at
  `(pixelX + shift*2, pixelY)` with the native `ste.width × ste.height`.
  Recovering the sub-pixel `shift*2` that the EGA path bakes into the
  shifted bitmap lands the HD art exactly where the EGA frame sits;
  `originX/originY` are already folded into `sde->x/y` by `RF_AddSpriteDraw`.
  HD sprites are assumed to cover the native sprite's bounding box (so the
  manifest's `originX/originY` columns are informational for now).
- **White flash preserved.** The damage/collect flash (`maskOnly` sprites,
  EGA `VH_DrawShiftedSpriteMask`) is reproduced by `drawQuad`'s new
  `maskOnly` path: fixed-function texture combiners output a solid-white RGB
  while keeping the texture's alpha (`GL_COMBINE`: `REPLACE(PRIMARY_COLOR)`
  for RGB, `REPLACE(TEXTURE)` for alpha) — no extra shader. Without this the
  HD sprite would draw normally over the EGA silhouette and the flash would
  be lost.

**Z-order caveat.** HD and EGA occupy two composited layers, not one
interleaved stack: the entire HD layer sits over the entire upscaled-EGA
frame. Within the HD layer, order is correct. But a *mixed* scene (e.g. an
HD sprite that should pass behind an un-replaced EGA fore tile) can't
interleave across the layer boundary — the HD sprite will be over the EGA
tile. The fix is to provide HD art for the neighbouring chunks too; a
"complete pack" has no mixed-layer seams. Documented as a pack-authoring
contract.

## 12. Phase 4: shipped — HD UI bitmaps & sprites

UI artwork now uses the same chunk-replacement path. The big visible win
is replacing static screens (title, game-over, paddle-game backgrounds,
status bar bitmaps) with HD versions.

- **VH hooks.** `src/id_vh.c` now submits HD draws alongside the EGA blit
  in `VH_DrawBitmap`, `VH_DrawMaskedBitmap`, `VH_DrawSprite`, and
  `VH_DrawSpriteMask` — each at the same screen-pixel rect the EGA call
  uses. The compositor early-outs if HD is off or no replacement exists,
  so these hooks are free for non-HD builds.
- **No double-submission with RFL sprites.** `VH_DrawShiftedSprite` and
  `VH_DrawShiftedSpriteMask` are intentionally **not** hooked: they are
  called only from `RFL_DrawSpriteList`, which already submits each
  visible sprite once via `VL_HD_DrawChunk` with the sub-pixel-recovered
  position. Hooking them here as well would double-draw HD sprites.
- **Begin/end gate dropped.** `VL_HD_BeginFrame` / `VL_HD_EndFrame` are
  now documentation no-ops in the GL backend: the draw list resets after
  every Present-time flush, not at BeginFrame. This lets UI code that
  doesn't go through `RF_Refresh` (menus, the title sequence, the
  paddle-game intro) accumulate HD draws and have them flushed by
  whatever `VL_Present` call eventually occurs.
- **Mask colour collapsed to white.** `VH_DrawSpriteMask(... colour)`
  takes an EGA colour for the silhouette; the HD path always draws it
  white (the existing `maskOnly` combiner state). Vanilla callers use
  colour 15 nearly exclusively, so this is a sane simplification.

**Fonts are deliberately out of scope for Phase 4.** `VH_DrawPropString`
/ `VH_DrawPropChar` push one 1-bpp glyph per character via
`VL_1bppToScreen`, with per-glyph variable widths. Doing them well needs
either a glyph-atlas HD font texture or per-codepoint manifest entries;
the existing output filters (Phase 0) already upscale EGA fonts
acceptably. Deferred to a future phase with the right tooling.

## 13. Phase 5: shipped — tooling & docs

A pack is now authorable without hand-tooling.

- **`/DUMPGFX [outdir]` exporter.** `src/ck_dumpgfx.c` decodes every tile
  (16×16 bg + fg), bitmap, masked bitmap, and sprite chunk straight from the
  planar EGA data (via `VL_EGARGBColorTable`) and writes each as a native-size
  32-bit BMP, plus a ready-to-edit `omnispeak_hd.txt` manifest listing them
  all with correct absolute chunk numbers (and sprite origins). It runs
  **before** the video backend starts (`ck_main.c` handles the switch right
  after the episode is resolved, then exits), so it works headless.
- **Backend-free & alpha-correct.** The dumper writes a `BITMAPV4HEADER`
  32-bit BGRA BMP by hand (`SDL_SaveBMP` drops alpha), with masked
  tiles/sprites carrying transparency from the EGA mask plane (set mask bit =
  transparent, matching `VL_MaskedBlitToPAL8`). A round-trip test confirmed the
  output reloads through the Phase 1 `SDL_LoadBMP` path with alpha intact.
- **Validation.** Loading a pack with `/HD` already reports per-entry problems:
  each unreadable image, decode failure, or missing manifest logs a warning and
  is skipped, and a final "Loaded N HD asset(s)" line confirms the count — so
  enabling the pack doubles as the manifest check.
- **Docs.** `doc/modding.md` gains an "HD Graphics Packs" section covering
  enabling, the manifest format, the `/DUMPGFX` workflow, the layering caveat,
  and current limitations.

**Fixed in passing:** the sprite-table `width` field is a *byte* width; the
Phase 3/4 sprite draws were missing the `*8` and would have rendered HD sprites
at one-eighth width. Corrected in `id_rf.c` and `id_vh.c`.

**Deferred (not blocking a pack):** PNG input (BMP only today) and HD fonts.

## 14. Summary

The engine is cleanly layered enough that an HD remaster does **not** require
touching game logic. The plan is: (1) add edge-aware shader upscaling now for an
immediate win; (2) build an output-resolution HD compositor behind
`VL_Backend`; (3) hook the existing tile/sprite draw sites
(`RF_RenderTile16`, `RFL_DrawSpriteList`, `RFL_RenderForeTiles`) to draw HD
replacements when an asset pack provides them, falling back to upscaled EGA
otherwise. The dominant cost is producing the art, which the per-asset fallback
lets us do incrementally.
