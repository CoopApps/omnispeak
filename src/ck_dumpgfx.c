/*
Omnispeak: A Commander Keen Reimplementation
Copyright (C) 2012 David Gow <david@ingeniumdigital.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

// HD asset-pack starter tool. Decodes every graphics chunk (tiles, bitmaps,
// sprites) out of the EGAGRAPH and writes each as a 32-bit BMP plus a starter
// manifest, ready to upscale/redraw into an HD pack (see doc/hd-remaster.md
// and doc/modding.md). Invoked with the /DUMPGFX [outdir] command line switch.
//
// This is deliberately backend-free: it decodes the planar EGA data directly
// using VL_EGARGBColorTable, so it needs only CA (graphics) to be started, not
// a video backend.

#include "ck_def.h"
#include "id_ca.h"
#include "id_fs.h"
#include "id_vh.h"
#include "id_vl.h"
#include "ck_cross.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Minimal little-endian BMP writer (BITMAPV4HEADER, 32-bit BGRA) ----------
// We hand-write the header so alpha survives the round-trip (SDL_SaveBMP drops
// it). Pixels are supplied as 0xAARRGGBB words; the file stores them as BGRA.

static void CKL_PutU16(FILE *f, uint16_t v)
{
	fputc(v & 0xFF, f);
	fputc((v >> 8) & 0xFF, f);
}

static void CKL_PutU32(FILE *f, uint32_t v)
{
	fputc(v & 0xFF, f);
	fputc((v >> 8) & 0xFF, f);
	fputc((v >> 16) & 0xFF, f);
	fputc((v >> 24) & 0xFF, f);
}

// Writes a top-down 32-bit BGRA BMP. 'pixels' has w*h entries, 0xAARRGGBB.
static bool CKL_WriteBMP(const char *path, const uint32_t *pixels, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;

	const uint32_t headerSize = 14 + 108; // file header + BITMAPV4HEADER
	const uint32_t imageSize = (uint32_t)w * (uint32_t)h * 4;

	// BITMAPFILEHEADER
	fputc('B', f);
	fputc('M', f);
	CKL_PutU32(f, headerSize + imageSize);
	CKL_PutU32(f, 0);
	CKL_PutU32(f, headerSize);

	// BITMAPV4HEADER
	CKL_PutU32(f, 108);		    // biSize
	CKL_PutU32(f, (uint32_t)w);	    // biWidth
	CKL_PutU32(f, (uint32_t)(-h));	    // biHeight (negative = top-down)
	CKL_PutU16(f, 1);		    // biPlanes
	CKL_PutU16(f, 32);		    // biBitCount
	CKL_PutU32(f, 3);		    // biCompression = BI_BITFIELDS
	CKL_PutU32(f, imageSize);	    // biSizeImage
	CKL_PutU32(f, 2835);		    // biXPelsPerMeter (~72 DPI)
	CKL_PutU32(f, 2835);		    // biYPelsPerMeter
	CKL_PutU32(f, 0);		    // biClrUsed
	CKL_PutU32(f, 0);		    // biClrImportant
	CKL_PutU32(f, 0x00FF0000);	    // red mask
	CKL_PutU32(f, 0x0000FF00);	    // green mask
	CKL_PutU32(f, 0x000000FF);	    // blue mask
	CKL_PutU32(f, 0xFF000000);	    // alpha mask
	CKL_PutU32(f, 0x73524742);	    // bV4CSType = 'sRGB'
	for (int i = 0; i < 9; ++i)	    // CIEXYZTRIPLE endpoints (unused)
		CKL_PutU32(f, 0);
	CKL_PutU32(f, 0);		    // gamma red
	CKL_PutU32(f, 0);		    // gamma green
	CKL_PutU32(f, 0);		    // gamma blue

	// Pixel data, stored as BGRA, top-down.
	for (int i = 0; i < w * h; ++i)
	{
		uint32_t p = pixels[i];
		fputc((p) & 0xFF, f);	    // B
		fputc((p >> 8) & 0xFF, f);  // G
		fputc((p >> 16) & 0xFF, f); // R
		fputc((p >> 24) & 0xFF, f); // A
	}

	bool ok = !ferror(f);
	fclose(f);
	return ok;
}

// --- EGA planar -> RGBA decode -----------------------------------------------

// Decodes a planar EGA chunk into a freshly-malloc'd 0xAARRGGBB buffer.
// 'masked' selects the 5-plane (mask first) layout; otherwise 4 planes.
// pixelW must be a multiple of 8.
static uint32_t *CKL_DecodeChunk(const uint8_t *src, int pixelW, int h, bool masked)
{
	int bytesPerPlane = (pixelW / 8) * h;
	const uint8_t *mask = masked ? src : NULL;
	const uint8_t *base = masked ? src + bytesPerPlane : src;
	const uint8_t *pb = base;
	const uint8_t *pg = pb + bytesPerPlane;
	const uint8_t *pr = pg + bytesPerPlane;
	const uint8_t *pi = pr + bytesPerPlane;

	uint32_t *out = (uint32_t *)malloc((size_t)pixelW * h * sizeof(uint32_t));
	if (!out)
		return NULL;

	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < pixelW; ++x)
		{
			int off = (y * pixelW + x) >> 3;
			int bit = 1 << (7 - ((y * pixelW + x) & 7));

			int idx = ((pi[off] & bit) ? 8 : 0) | ((pr[off] & bit) ? 4 : 0) |
				((pg[off] & bit) ? 2 : 0) | ((pb[off] & bit) ? 1 : 0);

			// A set mask bit means "transparent" (background shows through),
			// matching VL_MaskedBlitToPAL8.
			uint8_t a = 0xFF;
			if (mask && (mask[off] & bit))
				a = 0x00;

			uint8_t r = VL_EGARGBColorTable[idx][0];
			uint8_t g = VL_EGARGBColorTable[idx][1];
			uint8_t b = VL_EGARGBColorTable[idx][2];
			out[y * pixelW + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
		}
	}
	return out;
}

// --- Dump driver -------------------------------------------------------------

static FILE *ckl_manifest;
static int ckl_dumpCount;

// Decodes one chunk and writes "<prefix>_<chunk>.bmp" + a manifest line.
static void CKL_DumpOne(const char *outDir, const char *prefix, int chunk, int pixelW, int h, bool masked)
{
	if (pixelW <= 0 || h <= 0 || (pixelW & 7))
		return;

	CA_CacheGrChunk(chunk);
	const uint8_t *src = (const uint8_t *)ca_graphChunks[chunk];
	if (!src)
		return; // Empty/absent chunk (e.g. blank tiles) -- nothing to dump.

	uint32_t *rgba = CKL_DecodeChunk(src, pixelW, h, masked);
	if (!rgba)
		return;

	char fileName[64];
	snprintf(fileName, sizeof(fileName), "%s_%d.bmp", prefix, chunk);

	char fullPath[512];
	snprintf(fullPath, sizeof(fullPath), "%s/%s", outDir, fileName);

	if (CKL_WriteBMP(fullPath, rgba, pixelW, h))
	{
		// chunk file scale (scale=1; the dump is native EGA size, modders bump
		// it when they replace the BMP with a larger one).
		FS_PrintF(ckl_manifest, "%-6d %-28s 1\n", chunk, fileName);
		ckl_dumpCount++;
	}
	else
	{
		CK_Cross_LogMessage(CK_LOG_MSG_WARNING, "CK_DumpGfx: failed to write %s\n", fullPath);
	}
	free(rgba);
}

void CK_DumpGfx(const char *outDir)
{
	if (!outDir || !outDir[0])
		outDir = "hd_dump";

	// The dimension-table header chunks must be resident before we read them.
	CA_CacheGrChunk(ca_gfxInfoE.hdrBitmaps);
	CA_CacheGrChunk(ca_gfxInfoE.hdrMasked);
	CA_CacheGrChunk(ca_gfxInfoE.hdrSprites);

	char manifestPath[512];
	snprintf(manifestPath, sizeof(manifestPath), "%s/omnispeak_hd.txt", outDir);
	ckl_manifest = fopen(manifestPath, "wb");
	if (!ckl_manifest)
	{
		CK_Cross_LogMessage(CK_LOG_MSG_ERROR,
			"CK_DumpGfx: could not open '%s' for writing. Create the output "
			"directory first (default: ./hd_dump).\n",
			manifestPath);
		return;
	}

	FS_PrintF(ckl_manifest,
		"# Omnispeak HD asset manifest (generated by /DUMPGFX).\n"
		"# Replace each .bmp with a higher-resolution version and set the\n"
		"# scale column to its HD-pixels-per-EGA-pixel factor (e.g. 4).\n"
		"# Columns: chunk  file  scale  [originX originY]\n\n");

	ckl_dumpCount = 0;

	// Background tiles (16x16, unmasked).
	FS_PrintF(ckl_manifest, "# --- 16x16 background tiles ---\n");
	for (int i = 0; i < ca_gfxInfoE.numTiles16; ++i)
		CKL_DumpOne(outDir, "tile16", ca_gfxInfoE.offTiles16 + i, 16, 16, false);

	// Foreground tiles (16x16, masked).
	FS_PrintF(ckl_manifest, "\n# --- 16x16 foreground tiles ---\n");
	for (int i = 0; i < ca_gfxInfoE.numTiles16m; ++i)
		CKL_DumpOne(outDir, "tile16m", ca_gfxInfoE.offTiles16m + i, 16, 16, true);

	// Bitmaps (UI pictures, unmasked).
	FS_PrintF(ckl_manifest, "\n# --- bitmaps ---\n");
	for (int i = 0; i < ca_gfxInfoE.numBitmaps; ++i)
	{
		VH_BitmapTableEntry d = VH_GetBitmapTableEntry(i);
		CKL_DumpOne(outDir, "bitmap", ca_gfxInfoE.offBitmaps + i, d.width * 8, d.height, false);
	}

	// Masked bitmaps (status bar etc). The dimension accessor is private to
	// id_vh.c, so read the header table directly (same width/height layout as
	// the unmasked bitmap table).
	FS_PrintF(ckl_manifest, "\n# --- masked bitmaps ---\n");
	const uint16_t *maskedTable = (const uint16_t *)ca_graphChunks[ca_gfxInfoE.hdrMasked];
	for (int i = 0; maskedTable && i < ca_gfxInfoE.numMasked; ++i)
	{
		int w = maskedTable[i * 2], h = maskedTable[i * 2 + 1];
		CKL_DumpOne(outDir, "mbitmap", ca_gfxInfoE.offMasked + i, w * 8, h, true);
	}

	// Sprites (masked; the raw chunk is the unshifted frame). originX/originY
	// are emitted as a comment so authors can keep HD hotspots aligned.
	FS_PrintF(ckl_manifest, "\n# --- sprites ---\n");
	for (int i = 0; i < ca_gfxInfoE.numSprites; ++i)
	{
		VH_SpriteTableEntry s = VH_GetSpriteTableEntry(i);
		int chunk = ca_gfxInfoE.offSprites + i;
		if (s.width <= 0 || s.height <= 0)
			continue;
		// The unshifted frame lives at the start of the cached VH_ShiftedSprite.
		CA_CacheGrChunk(chunk);
		VH_ShiftedSprite *shifted = (VH_ShiftedSprite *)ca_graphChunks[chunk];
		if (!shifted)
			continue;
		uint32_t *rgba = CKL_DecodeChunk(shifted->data, s.width * 8, s.height, true);
		if (!rgba)
			continue;
		char fileName[64];
		snprintf(fileName, sizeof(fileName), "sprite_%d.bmp", chunk);
		char fullPath[512];
		snprintf(fullPath, sizeof(fullPath), "%s/%s", outDir, fileName);
		if (CKL_WriteBMP(fullPath, rgba, s.width * 8, s.height))
		{
			FS_PrintF(ckl_manifest, "%-6d %-28s 1   %d %d\n", chunk, fileName, s.originX, s.originY);
			ckl_dumpCount++;
		}
		free(rgba);
	}

	fclose(ckl_manifest);
	ckl_manifest = NULL;
	CK_Cross_LogMessage(CK_LOG_MSG_NORMAL, "CK_DumpGfx: wrote %d image(s) and manifest to '%s'.\n", ckl_dumpCount, outDir);
}
