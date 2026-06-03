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

// The HD asset manager. This is the backend-agnostic half of the optional
// high-resolution rendering path: it loads an HD asset pack (a manifest plus
// image files, kept alongside the original Keen data) and maps original
// graphics chunk numbers to their HD replacements. The actual GPU upload and
// drawing is delegated to the active backend's VL_HDBackend (see id_vl.h).
//
// This module is inert unless the "vl_hdAssets" config key (or the /HD command
// line switch) is set AND the active backend advertises HD support AND a
// manifest is present. With no manifest, behaviour is identical to a normal
// EGA build. See doc/hd-remaster.md.

#ifndef ID_VL_HD_H
#define ID_VL_HD_H

#include <stdbool.h>

// An HD image that replaces a single original graphics chunk.
typedef struct VL_HDImage
{
	int chunk;        // The original (absolute) graphics chunk number it replaces.
	void *image;      // Opaque, backend-owned GPU resource (from loadImage).
	int hdW, hdH;     // The HD image's own pixel dimensions.
	int scale;        // HD pixels per original EGA pixel (e.g. 4).
	int originX;      // Sprite hotspot in original EGA pixels (0 for tiles/bitmaps).
	int originY;
} VL_HDImage;

// Loads the HD asset pack if enabled. Safe to call when disabled (no-op).
void VL_HD_Startup(void);

// Frees all loaded HD assets.
void VL_HD_Shutdown(void);

// True if HD assets were successfully enabled and loaded.
bool VL_HD_IsEnabled(void);

// Returns the HD replacement for an original chunk, or NULL if none exists
// (or HD is disabled). The returned pointer is owned by the manager.
const VL_HDImage *VL_HD_GetImage(int chunk);

#endif // ID_VL_HD_H
