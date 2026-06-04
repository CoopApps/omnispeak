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

#include "id_vl_hd.h"
#include "id_vl.h"
#include "id_fs.h"
#include "id_cfg.h"
#include "id_us.h"
#include "ck_cross.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Set from the config key "vl_hdAssets" or the /HD command line switch
// (see id_vl.c). When false, this whole module is inert.
extern bool vl_hdAssets;

static bool vl_hd_enabled = false;

// Loaded HD images, kept sorted by chunk number for bsearch lookups.
static VL_HDImage *vl_hd_images = NULL;
static int vl_hd_numImages = 0;
static int vl_hd_capImages = 0;

static int VL_HD_CompareImages(const void *a, const void *b)
{
	return ((const VL_HDImage *)a)->chunk - ((const VL_HDImage *)b)->chunk;
}

// Reads an entire Keen-path file into a freshly malloc'd buffer. Returns NULL
// (and leaves *outLen untouched) on failure; caller frees the buffer.
static void *VL_HD_ReadWholeFile(const char *fileName, int *outLen)
{
	FS_File file = FS_OpenKeenFile(fileName);
	if (!FS_IsFileValid(file))
		return NULL;

	size_t len = FS_GetFileSize(file);
	void *buf = malloc(len ? len : 1);
	if (!buf)
	{
		FS_CloseFile(file);
		return NULL;
	}

	size_t got = FS_Read(buf, 1, len, file);
	FS_CloseFile(file);
	if (got != len)
	{
		free(buf);
		return NULL;
	}
	*outLen = (int)len;
	return buf;
}

static void VL_HD_AddImage(const VL_HDImage *img)
{
	if (vl_hd_numImages == vl_hd_capImages)
	{
		int newCap = vl_hd_capImages ? vl_hd_capImages * 2 : 64;
		VL_HDImage *grown = (VL_HDImage *)realloc(vl_hd_images, newCap * sizeof(VL_HDImage));
		if (!grown)
			Quit("VL_HD: Out of memory growing the HD image table.");
		vl_hd_images = grown;
		vl_hd_capImages = newCap;
	}
	vl_hd_images[vl_hd_numImages++] = *img;
}

// Parses one manifest line into *img. Lines look like:
//   <chunk> <file> [scale] [originX] [originY]
// Blank lines and lines starting with '#' are ignored. Returns true if a
// (syntactically valid) entry was parsed.
static bool VL_HD_ParseLine(char *line, VL_HDImage *img)
{
	// Strip a trailing comment and surrounding whitespace.
	char *hash = strchr(line, '#');
	if (hash)
		*hash = '\0';

	char fileName[256] = {0};
	int chunk = -1, scale = 0, originX = 0, originY = 0;
	int n = sscanf(line, " %d %255s %d %d %d", &chunk, fileName, &scale, &originX, &originY);
	if (n < 2 || chunk < 0 || !fileName[0])
		return false;

	int len = 0;
	void *fileData = VL_HD_ReadWholeFile(fileName, &len);
	if (!fileData)
	{
		CK_Cross_LogMessage(CK_LOG_MSG_WARNING, "VL_HD: Could not open HD image '%s' (chunk %d), skipping.\n", fileName, chunk);
		return false;
	}

	int w = 0, h = 0;
	void *image = VL_GetCurrentBackend()->hd->loadImage(fileData, len, &w, &h);
	free(fileData);
	if (!image)
	{
		CK_Cross_LogMessage(CK_LOG_MSG_WARNING, "VL_HD: Could not decode HD image '%s' (chunk %d), skipping.\n", fileName, chunk);
		return false;
	}

	img->chunk = chunk;
	img->image = image;
	img->hdW = w;
	img->hdH = h;
	img->scale = (scale > 0) ? scale : 1;
	img->originX = originX;
	img->originY = originY;
	return true;
}

void VL_HD_Startup(void)
{
	vl_hd_enabled = false;

	// Allow the config file to enable HD too (the /HD switch sets vl_hdAssets
	// in VL_Startup before we are called).
	if (!vl_hdAssets)
		vl_hdAssets = CFG_GetConfigBool("vl_hdAssets", false);
	if (!vl_hdAssets)
		return;

	VL_Backend *backend = VL_GetCurrentBackend();
	if (!backend || !backend->hd || !backend->hd->hasHD || !backend->hd->hasHD())
	{
		CK_Cross_LogMessage(CK_LOG_MSG_WARNING, "VL_HD: HD assets requested, but the active renderer backend has no HD support. Falling back to EGA.\n");
		return;
	}

	const char *manifestName = CFG_GetConfigString("vl_hdManifest", "omnispeak_hd.txt");
	int manifestLen = 0;
	char *manifest = (char *)VL_HD_ReadWholeFile(manifestName, &manifestLen);
	if (!manifest)
	{
		CK_Cross_LogMessage(CK_LOG_MSG_WARNING, "VL_HD: HD assets enabled, but manifest '%s' was not found in the Keen data path. Falling back to EGA.\n", manifestName);
		return;
	}

	// Walk the manifest line by line (it is not NUL-terminated on disk).
	char *cursor = manifest;
	char *end = manifest + manifestLen;
	while (cursor < end)
	{
		char *lineEnd = (char *)memchr(cursor, '\n', end - cursor);
		size_t lineLen = lineEnd ? (size_t)(lineEnd - cursor) : (size_t)(end - cursor);

		char lineBuf[320];
		size_t copyLen = lineLen < sizeof(lineBuf) - 1 ? lineLen : sizeof(lineBuf) - 1;
		memcpy(lineBuf, cursor, copyLen);
		lineBuf[copyLen] = '\0';

		VL_HDImage img;
		if (VL_HD_ParseLine(lineBuf, &img))
			VL_HD_AddImage(&img);

		cursor = lineEnd ? lineEnd + 1 : end;
	}
	free(manifest);

	if (vl_hd_numImages > 0)
		qsort(vl_hd_images, vl_hd_numImages, sizeof(VL_HDImage), VL_HD_CompareImages);

	vl_hd_enabled = true;
	CK_Cross_LogMessage(CK_LOG_MSG_NORMAL, "VL_HD: Loaded %d HD asset(s) from manifest '%s'.\n", vl_hd_numImages, manifestName);
}

void VL_HD_Shutdown(void)
{
	VL_Backend *backend = VL_GetCurrentBackend();
	if (backend && backend->hd && backend->hd->destroyImage)
	{
		for (int i = 0; i < vl_hd_numImages; ++i)
			backend->hd->destroyImage(vl_hd_images[i].image);
	}
	free(vl_hd_images);
	vl_hd_images = NULL;
	vl_hd_numImages = 0;
	vl_hd_capImages = 0;
	vl_hd_enabled = false;
}

bool VL_HD_IsEnabled(void)
{
	return vl_hd_enabled;
}

const VL_HDImage *VL_HD_GetImage(int chunk)
{
	if (!vl_hd_enabled || vl_hd_numImages == 0)
		return NULL;
	VL_HDImage key;
	key.chunk = chunk;
	return (const VL_HDImage *)bsearch(&key, vl_hd_images, vl_hd_numImages, sizeof(VL_HDImage), VL_HD_CompareImages);
}

void VL_HD_BeginFrame(void)
{
	if (!vl_hd_enabled)
		return;
	VL_Backend *backend = VL_GetCurrentBackend();
	if (backend && backend->hd && backend->hd->beginFrame)
		backend->hd->beginFrame();
}

void VL_HD_DrawChunk(int chunk, int bufferPxX, int bufferPxY, int egaW, int egaH, bool maskOnly)
{
	if (!vl_hd_enabled)
		return;
	const VL_HDImage *img = VL_HD_GetImage(chunk);
	if (!img)
		return;
	VL_Backend *backend = VL_GetCurrentBackend();
	if (backend && backend->hd && backend->hd->drawQuad)
		backend->hd->drawQuad(img->image, bufferPxX, bufferPxY, egaW, egaH, maskOnly);
}

void VL_HD_EndFrame(void)
{
	if (!vl_hd_enabled)
		return;
	VL_Backend *backend = VL_GetCurrentBackend();
	if (backend && backend->hd && backend->hd->endFrame)
		backend->hd->endFrame();
}
