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

#ifndef ID_VL_H
#define ID_VL_H

#include <stdbool.h>
#include <stdint.h>

/* Util Functions */

extern bool vl_screenFaded;
extern bool vl_isFullScreen;
extern bool vl_isAspectCorrected;
extern bool vl_isIntegerScaled;
extern bool vl_hasOverscanBorder;
extern int vl_swapInterval;
extern bool vl_hiddenCard; //TODO: Use this to enable even unwise fallbacks.
extern bool vl_noPan;

// EGA signal palettes (the 17th entry of each row is the overscan border color)
// NOTE: Vanilla Keen can modify some of these (e.g. the border color)
extern uint8_t vl_palette[6][17];
extern uint16_t vl_border_color;

#if 0
extern VL_EGAPaletteEntry VL_EGAPalette[16];
void VL_SetPalEntry(int id, uint8_t r, uint8_t g, uint8_t b);
#endif

void VL_SetPaletteByID(int id);
void VL_SetPalette(uint8_t *palette);
void VL_SetPaletteFromArray(intptr_t *palArray);
void VL_SetPaletteAndBorderColor(uint8_t *palette);
void VL_ColorBorder(uint16_t color);
void VL_SetDefaultPalette(void);
void VL_FadeToBlack(void);
void VL_FadeFromBlack(void);
//void VL_FadeToWhite(void); // Unused in vanilla Keen 5
//void VL_FadeFromWhite(void); // Unused in vanilla Keen 5
void VL_Clip(int *src_w, int *src_h, int *dst_x, int *dst_y, int dst_w, int dst_h);
void VL_UnmaskedToRGB(void *src, void *dest, int x, int y, int pitch, int w, int h);
void VL_MaskedToRGBA(void *src, void *dest, int x, int y, int pitch, int w, int h);
void VL_MaskedBlitToRGB(void *src, void *dest, int x, int y, int pitch, int w, int h);
void VL_MaskedBlitClipToRGB(void *src, void *dest, int x, int y, int pitch, int w, int h, int dw, int dh);
void VL_1bppToRGBA(void *src, void *dest, int x, int y, int pitch, int w, int h, int colour);
void VL_1bppXorWithRGB(void *src, void *dest, int x, int y, int pitch, int w, int h, int colour);
void VL_1bppBlitToRGB(void *src, void *dest, int x, int y, int pitch, int w, int h, int colour);
void VL_1bppInvBlitToRGB(void *src, void *dest, int x, int y, int pitch, int w, int h, int colour);
//TODO: 1bppInvBlitClipToRGB

void VL_UnmaskedToPAL8(void *src, void *dest, int x, int y, int pitch, int w, int h);
void VL_UnmaskedToPAL8_PM(void *src, void *dest, int x, int y, int pitch, int w, int h, int mapmask);
void VL_MaskedToPAL8(void *src, void *dest, int x, int y, int pitch, int w, int h);
void VL_MaskedBlitToPAL8(void *src, void *dest, int x, int y, int pitch, int w, int h);
void VL_MaskedBlitClipToPAL8(void *src, void *dest, int x, int y, int pitch, int w, int h, int dw, int dh);
void VL_1bppToPAL8(void *src, void *dest, int x, int y, int pitch, int w, int h, int colour);
void VL_1bppToPAL8_PM(void *src, void *dest, int x, int y, int pitch, int w, int h, int colour, int mapmask);
void VL_1bppXorWithPAL8(void *src, void *dest, int x, int y, int pitch, int w, int h, int colour);
void VL_1bppBlitToPAL8(void *src, void *dest, int x, int y, int pitch, int w, int h, int colour);
void VL_1bppInvBlitToPAL8(void *src, void *dest, int x, int y, int pitch, int w, int h, int colour);
void VL_1bppInvBlitClipToPAL8(void *src, void *dest, int x, int y, int pitch, int w, int h, int dw, int dh, int colour);

int VL_MemUsed();
int VL_NumSurfaces();

typedef enum VL_SurfaceUsage
{
	VL_SurfaceUsage_Default,
	VL_SurfaceUsage_FrameBuffer,
	VL_SurfaceUsage_FrontBuffer,
	VL_SurfaceUsage_Sprite
} VL_SurfaceUsage;

// Optional high-resolution ("HD") rendering interface. A backend that can
// render replacement HD artwork advertises it by setting the 'hd' field of
// its VL_Backend (NULL means no HD support, which is the default). See
// id_vl_hd.c and doc/hd-remaster.md.
typedef struct VL_HDBackend
{
	// Returns true if HD rendering is actually usable on this backend/system.
	bool (*hasHD)(void);
	// Decodes an in-memory image file (currently 32-bit BMP) and uploads it as
	// a GPU resource. Returns an opaque, backend-owned handle (NULL on failure)
	// and writes the image's pixel dimensions to *outW/*outH.
	void *(*loadImage)(const void *fileData, int dataLen, int *outW, int *outH);
	// Frees an image previously returned by loadImage.
	void (*destroyImage)(void *image);

	// Per-frame HD compositor. The backend records draws between begin/end and
	// composites them over the upscaled EGA frame inside its present() pass.
	// Coordinates are in EGA buffer pixels (same space as VL_SetScrollCoords);
	// scrolling is applied by the backend using the scroll passed to present().
	void (*beginFrame)(void);
	// Draws a single HD image into an EGA-pixel rectangle of the buffer.
	// 'image' must be a handle previously returned by loadImage. When
	// 'maskOnly' is true the image is drawn as a solid-white silhouette
	// (its alpha is kept), matching the EGA white-flash mask path.
	void (*drawQuad)(void *image, int bufferPxX, int bufferPxY, int egaW, int egaH, bool maskOnly);
	void (*endFrame)(void);
} VL_HDBackend;

typedef struct VL_Backend
{
	void (*setVideoMode)(int mode);
	void *(*createSurface)(int w, int h, VL_SurfaceUsage usage);
	void (*destroySurface)(void *surface);
	long (*getSurfaceMemUse)(void *surface);
	void (*getSurfaceDimensions)(void *surface, int *w, int *h);
	void (*refreshPaletteAndBorderColor)(void *screen);
	int (*surfacePGet)(void *screen, int x, int y);
	void (*surfaceRect)(void *dst_surface, int x, int y, int w, int h, int colour);
	void (*surfaceRect_PM)(void *dst_surface, int x, int y, int w, int h, int colour, int mapmask);
	void (*surfaceToSurface)(void *src_surface, void *dst_surface, int x, int y, int sx, int sy, int sw, int sh);
	void (*surfaceToSelf)(void *surface, int x, int y, int sx, int sy, int sw, int sh);
	void (*unmaskedToSurface)(void *src, void *dst_surface, int x, int y, int w, int h);
	void (*unmaskedToSurface_PM)(void *src, void *dst_surface, int x, int y, int w, int h, int mapmask);
	void (*maskedToSurface)(void *src, void *dst_surface, int x, int y, int w, int h);
	void (*maskedBlitToSurface)(void *src, void *dst_surface, int x, int y, int w, int h);
	void (*bitToSurface)(void *src, void *dst_surface, int x, int y, int w, int h, int colour);
	void (*bitToSurface_PM)(void *src, void *dst_surface, int x, int y, int w, int h, int colour, int mapmask);
	void (*bitXorWithSurface)(void *src, void *dst_surface, int x, int y, int w, int h, int colour);
	void (*bitBlitToSurface)(void *src, void *dst_surface, int x, int y, int w, int h, int colour);
	void (*bitInvBlitToSurface)(void *src, void *dst_surface, int x, int y, int w, int h, int colour);
	void (*scrollSurface)(void *surface, int x, int y);
	void (*present)(void *surface, int scrollXpx, int scrollYpx, bool singleBuffered);
	int (*getActiveBufferId)(void *surface);
	int (*getNumBuffers)(void *surface);
	void (*syncBuffers)(void *surface);
	void (*updateRect)(void *surface, int x, int y, int w, int h);
	void (*flushParams)();
	void (*waitVBLs)(int vbls);
	// Optional HD interface. NULL on backends without HD support. Placed last
	// so existing positional initializers (which omit it) zero-fill it to NULL.
	struct VL_HDBackend *hd;
} VL_Backend;

void VL_InitScreen(void);
void VL_Startup();
void VL_Shutdown();
void VL_ResizeScreen(int w, int h);
void VL_SetParams(bool isFullScreen, bool isAspectCorrected, bool hasOverscan, bool isIntegerScaled);
void VL_ToggleFullscreen();
void VL_ToggleAspect();
void VL_ToggleBorder();
void VL_ToggleInteger();
void VL_ToggleVSync();
void *VL_CreateSurface(int w, int h);
void VL_DestroySurface(void *surf);
void *VL_SetScreen(void *surf);
int VL_SurfacePGet(void *surf, int x, int y);
void VL_SurfaceRect(void *dst, int x, int y, int w, int h, int colour);
void VL_ScreenRect(int x, int y, int w, int h, int colour);
void VL_ScreenRect_PM(int x, int y, int w, int h, int colour);
void VL_ScreenToScreen(int x, int y, int sx, int sy, int sw, int sh);
void VL_SurfaceToSurface(void *src, void *dst, int x, int y, int sx, int sy, int sw, int sh);
void VL_SurfaceToScreen(void *src, int x, int y, int sx, int sy, int sw, int sh);
void VL_SurfaceToSelf(void *surf, int x, int y, int sx, int sy, int sw, int sh);
void VL_UnmaskedToSurface(void *src, void *dest, int x, int y, int w, int h);
void VL_UnmaskedToScreen(void *src, int x, int y, int w, int h);
void VL_UnmaskedToScreen_PM(void *src, int x, int y, int w, int h);
void VL_MaskedToSurface(void *src, void *dest, int x, int y, int w, int h);
void VL_MaskedBlitToSurface(void *src, void *dest, int x, int y, int w, int h);
void VL_MaskedToScreen(void *src, int x, int y, int w, int h);
void VL_MaskedBlitToScreen(void *src, int x, int y, int w, int h);
void VL_1bppToScreen(void *src, int x, int y, int w, int h, int colour);
void VL_1bppToScreen_PM(void *src, int x, int y, int w, int h, int colour);
void VL_1bppXorWithScreen(void *src, int x, int y, int w, int h, int colour);
void VL_1bppBlitToScreen(void *src, int x, int y, int w, int h, int colour);
void VL_1bppInvBlitToScreen(void *src, int x, int y, int w, int h, int colour);
void VL_ScrollScreen(int x, int y);

void VL_DelayTics(int tics);
int VL_GetTics(int wait);
void VL_Yield();
void VL_SetScrollCoords(int x, int y);
int VL_GetScrollX(void);
int VL_GetScrollY(void);
void VL_ClearScreen(int colour);
void VL_SetMapMask(int mapmask);
int VL_GetActiveBuffer();
int VL_GetNumBuffers();
void VL_FixRefreshBuffer();
void VL_UpdateRect(int x, int y, int w, int h);
void VL_SwapOnNextPresent();
void VL_Present();

VL_Backend *VL_Impl_GetBackend(void);

// Returns the backend selected at startup (valid after VL_InitScreen).
VL_Backend *VL_GetCurrentBackend(void);

#endif //ID_VL_H
