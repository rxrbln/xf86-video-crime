/* $NetBSD: crime.h,v 1.8 2022/07/15 04:30:05 mrg Exp $ */
/*
 * Copyright (c) 2008 Michael Lorenz
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    - Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    - Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>

/*
 * Backend selection: native wscons on NetBSD/OpenBSD, or the Linux
 * fbdev (gbefb) console device plus /dev/mem for the rendering engine.
 */
#ifdef HAVE_DEV_WSCONS_WSCONSIO_H
#define CRIME_WSCONS 1
#include <dev/wscons/wsconsio.h>
#else
#include <linux/fb.h>
#endif

#include "crmfbreg.h"

#include "xf86.h"
#include "xf86_OSproc.h"
#include "compat-api.h"

#if ABI_VIDEODRV_VERSION < SET_ABI_VERSION(25, 2) 
#include "xf86RamDac.h" 
#else  
#include "xf86Cursor.h"  
#endif
#include "xaa.h"

#ifndef CRIME_H
#define CRIME_H

/*#define CRIME_DEBUG*/

#define CRIME_DEBUG_LINES		0x00000001
#define CRIME_DEBUG_BITBLT		0x00000002
#define CRIME_DEBUG_RECTFILL		0x00000004
#define CRIME_DEBUG_IMAGEWRITE		0x00000008
#define CRIME_DEBUG_COLOUREXPAND	0x00000010
#define CRIME_DEBUG_CLIPPING		0x00000020
#define CRIME_DEBUG_SYNC		0x00000040
#define CRIME_DEBUG_XRENDER		0x00000080
#define CRIME_DEBUG_IMAGEREAD		0x00000100
#define CRIME_DEBUG_ALL			0xffffffff
#define CRIME_DEBUG_MASK 0

#ifdef CRIME_DEBUG
#define LOG(x) if (x & CRIME_DEBUG_MASK) xf86Msg(X_ERROR, "%s\n", __func__)
#define DONE(x) if (x & CRIME_DEBUG_MASK) \
		 xf86Msg(X_ERROR, "%s done\n", __func__)
#else
#define LOG(x)
#define DONE(x)
#endif

/* physical location of the CRIME rendering engine on the O2 */
#define CRIME_ENGINE_PHYS	0x15000000
#define CRIME_ENGINE_SIZE	0x5000
#define CRIME_LINEAR_PHYS	0x15010000
#define CRIME_LINEAR_SIZE	0x10000

/* the GBE display backend, home of the CRMFB_* registers (and cursor) */
#define CRIME_GBE_PHYS		0x16000000
#define CRIME_GBE_SIZE		0x00080000

/* GBE/CRIME framebuffer tile: 64kB, 128x128 pixels at 32bpp */
#define CRIME_TILE_SIZE		0x10000
#define CRIME_TILE_MASK		(CRIME_TILE_SIZE - 1)

/*
 * Frame buffer layout characteristics, backend independent.  On Linux
 * width/height describe the virtual (largest mode) area the tiled
 * framebuffer is laid out for; the currently scanned out mode may be
 * smaller (mode_width/mode_height below).
 */
struct crime_fbinfo {
	unsigned int		width;
	unsigned int		height;
	unsigned int		depth;
};

/* private data */
typedef struct {
	int			fd; /* file descriptor of open device */
	struct crime_fbinfo	info; /* frame buffer characteristics */
	Bool			HWCursor;
	CloseScreenProcPtr	CloseScreen;
	EntityInfoPtr		pEnt;

	int			vram_lines; /* on-screen + offscreen height */

#ifdef CRIME_WSCONS
	struct wsdisplay_cursor cursor;
	int			maskoffset;
#else
	int			memfd; /* /dev/mem for the engine mapping */
	volatile uint32_t	*gbe;  /* GBE display backend registers */

	/*
	 * The Linux gbefb driver knows nothing about the CRIME rendering
	 * engine, so unlike on NetBSD we must set up the engine's TLBs
	 * and switch the GBE scanout to the native tiled layout ourselves.
	 * All buffers are carved out of the (physically contiguous) gbefb
	 * video memory.
	 */
	unsigned long		smem_start;  /* gbefb video memory */
	unsigned int		smem_len;
	struct fb_var_screeninfo var; /* current fbdev state, 32bpp */
	int			mode_width;  /* current video mode - may be */
	int			mode_height; /* smaller than the virtual fb */
	unsigned long		fb_phys;     /* tile aligned framebuffer */
	unsigned long		linear_phys; /* 128kB DMA staging buffer */
	unsigned long		table_phys;  /* GBE tile pointer table */
	volatile uint16_t	*table;      /* mapped tile pointer table */
	int			tiles_x;     /* screen width in tiles */
	int			tile_rows;   /* usable tile rows (max 16) */

	Bool			gbe_saved;   /* saved_* below are valid */
	uint32_t		saved_frm_control;
	uint32_t		saved_frm_tilesize;
	uint32_t		saved_frm_pixsize;
#endif
	xf86CursorInfoPtr	CursorInfoRec;
	OptionInfoPtr		Options;

	XAAInfoRecPtr		pXAA;
	unsigned char		*engine;
	char			*linear;
	void			*fb;
	unsigned char		*buffers[8];
	unsigned char		*expandbuffers[1];
	int			ux, uy, uw, uh, us, um;
	int			start, xdir, ydir;
	int			format;
	int			use_mte;
	int			cxa, cxe, cya, cye;
	uint32_t		expand[2048];
	uint32_t		pattern[8];
	uint32_t		alpha_color;
	int			texture_depth;
	unsigned char		*alpha_texture;
	void			*src, *msk;
} CrimeRec, *CrimePtr;

#define CRIMEPTR(p) ((CrimePtr)((p)->driverPrivate))

Bool CrimeSetupCursor(ScreenPtr);
int CrimeAccelInit(ScrnInfoPtr);

#endif
