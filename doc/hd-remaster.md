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

Introduce an asset pack loaded from a directory (e.g. `hd/`) alongside the game
data, gated by a config key (`vl_hdAssets`, parsed like `rf_minTics` in
`src/id_rf.c:614`) and/or a `/HD` command-line switch.

Manifest (`hd/manifest.json` or a flat text file to avoid a JSON dependency)
maps **original chunk numbers** to HD image files plus metadata:

```
# kind   chunk          file                   scale  originX originY
tile16    <bg tile id>   tiles/bg_0123.png       4      -        -
tile16m   <fg tile id>   tiles/fg_0048.png       4      -        -
sprite    <chunk>        sprites/keen_walk0.png  4      40       28
bitmap    <chunk>        ui/title.png            4      -        -
```

- `scale` is the HD-pixels-per-EGA-pixel factor (e.g. 4 → a 16×16 tile becomes
  64×64). A global default with per-asset overrides.
- For sprites, `originX/originY` are the HD-space hotspot, derived from the EGA
  sprite's `originX/originY` (`VH_GetSpriteTableEntry`, used in
  `src/id_rf.c:1298`) so the HD art lands at the correct game position.

Chunk numbers are stable and already symbolically named in `GFXCHUNK.CKx`
(`doc/modding.md:122`), which makes building the manifest tractable — a tool can
dump every chunk to PNG (see `omnispeak --dumpgfx`-style tooling / idGrab) to use
as a redraw/upscale base.

### 4.2 HD compositor surface

Add to `VL_Backend` (`src/id_vl.h:91`) an optional HD interface — keep it
separate from the EGA blit functions so existing backends compile unchanged:

```c
typedef struct VL_HDBackend {
    bool (*hasHD)(void);
    void *(*loadHDImage)(const char *path);          // -> GPU texture
    void  (*beginHDFrame)(int scrollXpx, int scrollYpx, float egaToScreenScale);
    void  (*drawHDQuad)(void *tex, int egaX, int egaY, int egaW, int egaH,
                        int shift /*0-3*/, bool maskOnly, int tintColor);
    void  (*endHDFrame)(void);
} VL_HDBackend;
```

The compositor renders into an offscreen FBO sized to the output resolution
(reusing the framebuffer-texture machinery already present —
`vl_sdl2gl_framebufferTexture`, `id_glFramebufferTexture2DEXT`,
`src/id_vl_sdl2gl.c:169`/`:603`). `egaToScreenScale` maps native EGA pixel coords
to FBO coords so HD quads land exactly where the EGA frame would have.

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

## 8. Summary

The engine is cleanly layered enough that an HD remaster does **not** require
touching game logic. The plan is: (1) add edge-aware shader upscaling now for an
immediate win; (2) build an output-resolution HD compositor behind
`VL_Backend`; (3) hook the existing tile/sprite draw sites
(`RF_RenderTile16`, `RFL_DrawSpriteList`, `RFL_RenderForeTiles`) to draw HD
replacements when an asset pack provides them, falling back to upscaled EGA
otherwise. The dominant cost is producing the art, which the per-asset fallback
lets us do incrementally.
