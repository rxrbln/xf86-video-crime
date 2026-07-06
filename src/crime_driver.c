/* $NetBSD: crime_driver.c,v 1.12 2016/08/16 01:27:46 mrg Exp $ */
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

/* a driver for the CRIME rendering engine found in SGI O2 workstations */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

/* all drivers need this */
#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Modes.h"

#include "mipointer.h"
#include "micmap.h"
#include "colormapst.h"
#include "xf86cmap.h"

/* for visuals */
#include "fb.h"

#ifdef XvExtension
#include "xf86xv.h"
#endif

#include "crime.h"

#ifdef CRIME_WSCONS
#define CRIME_DEFAULT_DEV "/dev/ttyE0"
#else
#define CRIME_DEFAULT_DEV "/dev/fb0"
#endif

/* Prototypes */
static pointer CrimeSetup(pointer, pointer, int *, int *);
static Bool CrimeGetRec(ScrnInfoPtr);
static void CrimeFreeRec(ScrnInfoPtr);
static const OptionInfoRec * CrimeAvailableOptions(int, int);
static void CrimeIdentify(int);
static Bool CrimeProbe(DriverPtr, int);
static Bool CrimePreInit(ScrnInfoPtr, int);
static Bool CrimeScreenInit(SCREEN_INIT_ARGS_DECL);
static Bool CrimeCloseScreen(CLOSE_SCREEN_ARGS_DECL);
static Bool CrimeEnterVT(VT_FUNC_ARGS_DECL);
static void CrimeLeaveVT(VT_FUNC_ARGS_DECL);
static Bool CrimeSwitchMode(SWITCH_MODE_ARGS_DECL);
static void CrimeLoadPalette(ScrnInfoPtr, int, int *, LOCO *, VisualPtr);
static Bool CrimeSaveScreen(ScreenPtr, int);
static void CrimeRestore(ScrnInfoPtr);

/* helper functions */
static int crime_open(const char *);
static pointer crime_mmap(size_t, off_t, int, int);

/*
 * This is intentionally screen-independent.  It indicates the binding
 * choice made in the first PreInit.
 */
static int pix24bpp = 0;

#define CRIME_VERSION		4000
#define CRIME_NAME		"crime"
#define CRIME_DRIVER_NAME	"crime"
#define CRIME_MAJOR_VERSION	0
#define CRIME_MINOR_VERSION	9

_X_EXPORT DriverRec CRIME = {
	CRIME_VERSION,
	CRIME_DRIVER_NAME,
	CrimeIdentify,
	CrimeProbe,
	CrimeAvailableOptions,
	NULL,
	0
};

/* Supported "chipsets" */
static SymTabRec CrimeChipsets[] = {
	{ 0, "crime" },
	{ -1, NULL }
};

/* Supported options */
typedef enum {
	OPTION_HW_CURSOR,
	OPTION_SW_CURSOR,
	OPTION_DEVICE
} CrimeOpts;

static const OptionInfoRec CrimeOptions[] = {
	{ OPTION_SW_CURSOR, "SWcursor",	OPTV_BOOLEAN,	{0}, FALSE },
	{ OPTION_HW_CURSOR, "HWcursor",	OPTV_BOOLEAN,	{0}, FALSE },
	{ OPTION_DEVICE,    "device",	OPTV_STRING,	{0}, FALSE },
	{ -1, NULL, OPTV_NONE, {0}, FALSE}
};

static XF86ModuleVersionInfo CrimeVersRec = {
	"crime",
	"The NetBSD Foundation",
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	CRIME_MAJOR_VERSION, CRIME_MINOR_VERSION, 0,
	ABI_CLASS_VIDEODRV,
	ABI_VIDEODRV_VERSION,
	MOD_CLASS_VIDEODRV,
	{0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData crimeModuleData = { &CrimeVersRec, CrimeSetup, NULL };

static pointer
CrimeSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;

	if (!setupDone) {
		setupDone = TRUE;
		xf86AddDriver(&CRIME, module, 0);
		return (pointer)1;
	} else {
		if (errmaj != NULL)
			*errmaj = LDR_ONCEONLY;
		return NULL;
	}
}

static Bool
CrimeGetRec(ScrnInfoPtr pScrn)
{

	if (pScrn->driverPrivate != NULL)
		return TRUE;

	pScrn->driverPrivate = xnfcalloc(sizeof(CrimeRec), 1);
	return TRUE;
}

static void
CrimeFreeRec(ScrnInfoPtr pScrn)
{

	if (pScrn->driverPrivate == NULL)
		return;
	free(pScrn->driverPrivate);
	pScrn->driverPrivate = NULL;
}

static const OptionInfoRec *
CrimeAvailableOptions(int chipid, int busid)
{
	return CrimeOptions;
}

static void
CrimeIdentify(int flags)
{
	xf86PrintChipsets(CRIME_NAME, "driver for CRIME framebuffer",
			  CrimeChipsets);
}

#define priv_open_device(n)	open(n,O_RDWR|O_NONBLOCK|O_EXCL)

/* Open the framebuffer device */
static int
crime_open(const char *dev)
{
	int fd = -1;

	/* try argument from XF86Config first */
	if (dev == NULL || ((fd = priv_open_device(dev)) == -1)) {
		/* second: environment variable */
		dev = getenv("XDEVICE");
		if (dev == NULL || ((fd = priv_open_device(dev)) == -1)) {
			/* last try: default device */
			dev = CRIME_DEFAULT_DEV;
			if ((fd = priv_open_device(dev)) == -1) {
				return -1;
			}
		}
	}
	return fd;
}

/*
 * Check that the opened device really is the CRIME/GBE, and gather the
 * framebuffer characteristics.
 */
static Bool
crime_check_device(int fd)
{
#ifdef CRIME_WSCONS
	int wstype;

	if (ioctl(fd, WSDISPLAYIO_GTYPE, &wstype) == -1)
		return FALSE;
	if (wstype != WSDISPLAY_TYPE_CRIME)
		return FALSE;
	return TRUE;
#else
	struct fb_fix_screeninfo fix;

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) == -1)
		return FALSE;
	/* the Linux gbefb driver identifies itself as "SGI GBE" */
	if (strncmp(fix.id, "SGI GBE", 7) != 0 &&
	    strncmp(fix.id, "GBE", 3) != 0) {
		xf86Msg(X_ERROR,
		    "crime: fb device is \"%s\", not an SGI GBE\n", fix.id);
		return FALSE;
	}
	return TRUE;
#endif
}

static Bool
crime_get_fbinfo(ScrnInfoPtr pScrn, CrimePtr fPtr)
{
	struct crime_fbinfo *info = &fPtr->info;
#ifdef CRIME_WSCONS
	struct wsdisplay_fbinfo wsinfo;

	if (ioctl(fPtr->fd, WSDISPLAYIO_GINFO, &wsinfo) == -1)
		return FALSE;
	info->width = wsinfo.width;
	info->height = wsinfo.height;
	info->depth = wsinfo.depth;
	return TRUE;
#else
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;

	if (ioctl(fPtr->fd, FBIOGET_VSCREENINFO, &var) == -1)
		return FALSE;

	/* the rendering engine paths all assume a 32bit framebuffer */
	if (var.bits_per_pixel != 32) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "switching fb from %d to 32 bits per pixel\n",
			   var.bits_per_pixel);
		var.bits_per_pixel = 32;
		var.activate = FB_ACTIVATE_NOW;
		if (ioctl(fPtr->fd, FBIOPUT_VSCREENINFO, &var) == -1 ||
		    ioctl(fPtr->fd, FBIOGET_VSCREENINFO, &var) == -1 ||
		    var.bits_per_pixel != 32) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "cannot switch fb to 32 bits per pixel, "
				   "boot with video=gbefb:depth:32\n");
			return FALSE;
		}
	}

	if (ioctl(fPtr->fd, FBIOGET_FSCREENINFO, &fix) == -1)
		return FALSE;

	info->width = var.xres;
	info->height = var.yres;
	info->depth = var.bits_per_pixel;
	fPtr->var = var;
	fPtr->smem_start = fix.smem_start;
	fPtr->smem_len = fix.smem_len;
	return TRUE;
#endif
}

#ifndef CRIME_WSCONS

#define WBFLUSH __asm volatile("sync" ::: "memory")

#define GBE_READ(fPtr, r)	((fPtr)->gbe[(r) >> 2])
#define GBE_WRITE(fPtr, r, v)	do {			\
	(fPtr)->gbe[(r) >> 2] = (v);			\
	WBFLUSH;					\
} while (0)

/*
 * Divide the gbefb video memory into the buffers the rendering engine
 * needs: a framebuffer of full 64kB tiles (visible screen plus
 * offscreen pixmap space), a 128kB linear DMA staging buffer and one
 * tile holding the GBE tile pointer table.  Only sizes here - the
 * physical location is resolved later, when the registers are mapped.
 */
static Bool
crime_compute_layout(ScrnInfoPtr pScrn, CrimePtr fPtr)
{
	int vis_rows, rows, tiles;

	fPtr->tiles_x = (fPtr->info.width + 127) >> 7;
	vis_rows = (fPtr->info.height + 127) >> 7;

	tiles = (fPtr->smem_len >> 16) - 3;
	rows = tiles / fPtr->tiles_x;
	if (rows > 16)		/* the engine TLB A is 16x16 tiles */
		rows = 16;
	if (tiles < 0 || rows < vis_rows) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "gbefb video memory too small: %d kB, but %dx%d "
			   "tiled needs %d kB; raise it with the gbefb "
			   "mem= option\n", fPtr->smem_len >> 10,
			   fPtr->info.width, fPtr->info.height,
			   ((vis_rows * fPtr->tiles_x + 3) * CRIME_TILE_SIZE)
			       >> 10);
		return FALSE;
	}

	fPtr->tile_rows = rows;
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "tiled fb layout: %d x %d tiles plus staging buffer\n",
		   fPtr->tiles_x, rows);
	return TRUE;
}

/*
 * Find the physical location of the gbefb video memory.  gbefb puts a
 * kernel virtual address into fix.smem_start, so that is useless here;
 * instead read the scanout tile table GBE is currently using (its
 * address is in FRM_CONTROL) - the entries hold the real physical
 * addresses of the 64kB blocks, in sequential order.
 */
static Bool
crime_resolve_phys(ScrnInfoPtr pScrn, CrimePtr fPtr)
{
	uint16_t entries[4];
	unsigned long tabaddr;
	int i, n;

	tabaddr = GBE_READ(fPtr, CRMFB_FRM_CONTROL) &
	    ~(unsigned long)((1 << CRMFB_FRM_CONTROL_TILEPTR_SHIFT) - 1);
	if (tabaddr == 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "no GBE tile table - gbefb not initialized?\n");
		return FALSE;
	}

	n = fPtr->smem_len >> 16;
	if (n > 4)
		n = 4;
	if (pread(fPtr->memfd, entries, n * sizeof(uint16_t),
	    (off_t)tabaddr) != (ssize_t)(n * sizeof(uint16_t))) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "cannot read GBE tile table at 0x%lx: %s\n",
			   tabaddr, strerror(errno));
		return FALSE;
	}
	for (i = 1; i < n; i++) {
		if (entries[i] != entries[0] + i) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "gbefb video memory is not contiguous\n");
			return FALSE;
		}
	}

	fPtr->fb_phys = (unsigned long)entries[0] << 16;
	fPtr->linear_phys = fPtr->fb_phys +
	    (unsigned long)fPtr->tile_rows * fPtr->tiles_x * CRIME_TILE_SIZE;
	fPtr->table_phys = fPtr->linear_phys + 2 * CRIME_TILE_SIZE;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "video memory at 0x%lx, staging buffer at 0x%lx\n",
		   fPtr->fb_phys, fPtr->linear_phys);
	return TRUE;
}

/*
 * Translate an X mode into fbdev timings on top of the current gbefb
 * pixel format.  gbefb computes its dot clock PLL from the requested
 * pixclock, so arbitrary modelines work as long as the PLL can reach
 * them.  gbefb cannot pan, so its virtual size always equals the mode;
 * the (possibly larger) X virtual area only exists in our tiled layout.
 */
static void
crime_mode_to_var(CrimePtr fPtr, DisplayModePtr mode,
    struct fb_var_screeninfo *var)
{
	*var = fPtr->var;
	var->xres = mode->HDisplay;
	var->yres = mode->VDisplay;
	var->xres_virtual = mode->HDisplay;
	var->yres_virtual = mode->VDisplay;
	var->xoffset = var->yoffset = 0;
	var->bits_per_pixel = 32;
	var->pixclock = mode->Clock ? 1000000000 / mode->Clock : 0;
	var->right_margin = mode->HSyncStart - mode->HDisplay;
	var->hsync_len = mode->HSyncEnd - mode->HSyncStart;
	var->left_margin = mode->HTotal - mode->HSyncEnd;
	var->lower_margin = mode->VSyncStart - mode->VDisplay;
	var->vsync_len = mode->VSyncEnd - mode->VSyncStart;
	var->upper_margin = mode->VTotal - mode->VSyncEnd;
	var->sync = 0;
	if (mode->Flags & V_PHSYNC)
		var->sync |= FB_SYNC_HOR_HIGH_ACT;
	if (mode->Flags & V_PVSYNC)
		var->sync |= FB_SYNC_VERT_HIGH_ACT;
	var->vmode = FB_VMODE_NONINTERLACED;
}

/* Build an X mode from the fbdev mode gbefb is currently programmed to */
static void
crime_var_to_mode(const struct fb_var_screeninfo *var, DisplayModePtr mode)
{
	mode->Clock = var->pixclock ? 1000000000 / var->pixclock : 0;
	mode->SynthClock = mode->Clock;
	mode->HDisplay = var->xres;
	mode->HSyncStart = mode->HDisplay + var->right_margin;
	mode->HSyncEnd = mode->HSyncStart + var->hsync_len;
	mode->HTotal = mode->HSyncEnd + var->left_margin;
	mode->VDisplay = var->yres;
	mode->VSyncStart = mode->VDisplay + var->lower_margin;
	mode->VSyncEnd = mode->VSyncStart + var->vsync_len;
	mode->VTotal = mode->VSyncEnd + var->upper_margin;
	mode->Flags = 0;
	mode->Flags |= var->sync & FB_SYNC_HOR_HIGH_ACT ?
	    V_PHSYNC : V_NHSYNC;
	mode->Flags |= var->sync & FB_SYNC_VERT_HIGH_ACT ?
	    V_PVSYNC : V_NVSYNC;
	xf86SetModeDefaultName(mode);
	xf86SetModeCrtc(mode, 0);
}

/* Ask gbefb whether it can do this mode, without changing anything */
static Bool
crime_test_mode(CrimePtr fPtr, DisplayModePtr mode)
{
	struct fb_var_screeninfo var;

	/* the engine TLB A addresses a 2048x2048 pixel area */
	if (mode->HDisplay > 2048 || mode->VDisplay > 2048)
		return FALSE;
	if (mode->Flags & (V_INTERLACE | V_DBLSCAN))
		return FALSE;

	crime_mode_to_var(fPtr, mode, &var);
	var.activate = FB_ACTIVATE_TEST;
	if (ioctl(fPtr->fd, FBIOPUT_VSCREENINFO, &var) == -1)
		return FALSE;
	return var.xres == (unsigned int)mode->HDisplay &&
	    var.yres == (unsigned int)mode->VDisplay &&
	    var.bits_per_pixel == 32;
}

/*
 * Build the mode list with the standard validation machinery: the
 * monitor's modelines and the server's default modes, filtered by the
 * frequency limits from xorg.conf (HorizSync/VertRefresh and Option
 * "MaxClock" in the Monitor section - conservative defaults apply if
 * none are given) and by what gbefb accepts.  The mode gbefb was
 * running at server start is always included and is the startup mode.
 */
static Bool
crime_select_modes(ScrnInfoPtr pScrn, CrimePtr fPtr)
{
	ClockRangePtr clockRanges;
	DisplayModePtr mode, builtin;
	int aperture;

	aperture = (int)fPtr->smem_len - 3 * CRIME_TILE_SIZE;
	if (aperture < CRIME_TILE_SIZE) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "gbefb video memory too small: %d kB; raise it "
			   "with the gbefb mem= option\n",
			   fPtr->smem_len >> 10);
		return FALSE;
	}

	/*
	 * The per-mode memory check in xf86ValidateModes works off
	 * videoRam (in kB), not the aperture argument; give it the
	 * usable gbefb memory.  The exact tiled layout is verified in
	 * crime_compute_layout afterwards.
	 */
	pScrn->videoRam = aperture / 1024;

	/* the GBE dot clock PLL: fvco 80-220 MHz, post-divide up to 8 */
	clockRanges = xnfcalloc(sizeof(ClockRange), 1);
	clockRanges->minClock = 10000;
	clockRanges->maxClock = 220000;
	clockRanges->clockIndex = -1;	/* programmable */
	clockRanges->interlaceAllowed = FALSE;
	clockRanges->doubleScanAllowed = FALSE;

	if (xf86ValidateModes(pScrn, pScrn->monitor->Modes,
	    pScrn->display->modes, clockRanges,
	    NULL, 128, 2048,		/* pitch range, pixels */
	    32,				/* pitchInc: any width, bits */
	    128, 2048,			/* height range */
	    pScrn->display->virtualX, pScrn->display->virtualY,
	    aperture, LOOKUP_BEST_REFRESH) == -1)
		return FALSE;
	xf86PruneDriverModes(pScrn);

	/*
	 * Make sure the mode detected on the fbdev at startup is in the
	 * pool, and start in it.
	 */
	builtin = NULL;
	if ((mode = pScrn->modes) != NULL) {
		do {
			if (mode->HDisplay == (int)fPtr->var.xres &&
			    mode->VDisplay == (int)fPtr->var.yres &&
			    builtin == NULL)
				builtin = mode;
			mode = mode->next;
		} while (mode != NULL && mode != pScrn->modes);
	}
	if (builtin == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "adding the current gbefb mode (%dx%d)\n",
			   fPtr->var.xres, fPtr->var.yres);
		builtin = xnfcalloc(sizeof(DisplayModeRec), 1);
		crime_var_to_mode(&fPtr->var, builtin);
		builtin->type = M_T_BUILTIN;
		builtin->status = MODE_OK;
		if (pScrn->modes == NULL) {
			builtin->prev = builtin->next = builtin;
			pScrn->modes = builtin;
		} else {
			builtin->next = pScrn->modes;
			builtin->prev = pScrn->modes->prev;
			pScrn->modes->prev->next = builtin;
			pScrn->modes->prev = builtin;
			pScrn->modes = builtin;
		}
		if (pScrn->virtualX < builtin->HDisplay)
			pScrn->virtualX = builtin->HDisplay;
		if (pScrn->virtualY < builtin->VDisplay)
			pScrn->virtualY = builtin->VDisplay;
	}
	pScrn->currentMode = builtin;

	if (pScrn->virtualX > 2048 || pScrn->virtualY > 2048) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "virtual screen %dx%d exceeds the 2048x2048 "
			   "engine limit\n", pScrn->virtualX, pScrn->virtualY);
		return FALSE;
	}

	/* the tiled framebuffer is laid out for the largest mode */
	pScrn->displayWidth = pScrn->virtualX;
	xf86SetCrtcForModes(pScrn, 0);
	xf86PrintModes(pScrn);
	return TRUE;
}

/* mode validation hook for xf86ValidateModes and the VidMode extension */
static ModeStatus
CrimeValidMode(ScrnInfoPtr pScrn, DisplayModePtr mode, Bool verbose,
    int flags)
{
	return crime_test_mode(CRIMEPTR(pScrn), mode) ? MODE_OK : MODE_BAD;
}
#endif

/* Switch the console device in and out of graphics mode */
static Bool
crime_set_gfx_mode(CrimePtr fPtr, Bool graphics)
{
#ifdef CRIME_WSCONS
	int wsmode = graphics ? WSDISPLAYIO_MODE_MAPPED : WSDISPLAYIO_MODE_EMUL;

	if (ioctl(fPtr->fd, WSDISPLAYIO_SMODE, &wsmode) == -1)
		return FALSE;
	return TRUE;
#else
	/*
	 * On Linux the server core handles KD_GRAPHICS/KD_TEXT on the VT;
	 * fbcon repaints the console when we switch away.
	 */
	return TRUE;
#endif
}

/* Map memory through the given file descriptor */
static pointer
crime_mmap(size_t len, off_t off, int fd, int ro)
{
	pointer mapaddr;

	/*
	 * try and make it private first, that way once we get it, an
	 * interloper, e.g. another server, can't get this frame buffer,
	 * and if another server already has it, this one won't.
	 */
	if (ro) {
		mapaddr = (pointer) mmap(NULL, len,
					 PROT_READ, MAP_SHARED,
					 fd, off);
	} else {
		mapaddr = (pointer) mmap(NULL, len,
					 PROT_READ | PROT_WRITE, MAP_SHARED,
					 fd, off);
	}
	if (mapaddr == (pointer) -1) {
		mapaddr = NULL;
	}
#ifdef CRIME_DEBUG
	ErrorF("mmap returns: addr %p len 0x%x\n", mapaddr, len);
#endif
	return mapaddr;
}

/* Map the rendering engine and its linear buffer */
static Bool
crime_map_engine(ScrnInfoPtr pScrn, CrimePtr fPtr)
{
	int fd;

#ifdef CRIME_WSCONS
	fd = fPtr->fd;
#else
	/*
	 * The Linux gbefb device only exposes the framebuffer itself, so
	 * the CRIME rendering engine is reached through /dev/mem.
	 */
	fPtr->memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fPtr->memfd == -1) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "cannot open /dev/mem for the CRIME engine: %s\n",
			   strerror(errno));
		return FALSE;
	}
	fd = fPtr->memfd;
#endif

	fPtr->engine = crime_mmap(CRIME_ENGINE_SIZE, CRIME_ENGINE_PHYS,
	    fd, 0);
	if (fPtr->engine == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "mmap CRIME engine: %s\n", strerror(errno));
		return FALSE;
	}

#ifdef CRIME_WSCONS
	/* the kernel transparently remaps this to the staging RAM */
	fPtr->linear = crime_mmap(CRIME_LINEAR_SIZE, CRIME_LINEAR_PHYS,
	    fd, 0);
	if (fPtr->linear == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "mmap CRIME linear buffer: %s\n", strerror(errno));
		return FALSE;
	}
#else
	/* GBE registers: needed to switch the scanout to tiled mode
	   (and for the hardware cursor) */
	fPtr->gbe = crime_mmap(CRIME_GBE_SIZE, CRIME_GBE_PHYS, fd, 0);
	if (fPtr->gbe == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "mmap GBE registers: %s\n", strerror(errno));
		return FALSE;
	}

	/* the GBE registers tell us where the video memory really is */
	if (!crime_resolve_phys(pScrn, fPtr))
		return FALSE;

	/*
	 * Map the staging buffer RAM directly, exactly like the NetBSD
	 * kernel's mmap handler hands it to the X driver there.  The CPU
	 * fills the buffers in RAM and only the engine reads them back
	 * through its LINEAR_A TLB; the engine's own linear aperture at
	 * 0x15010000 is not for CPU use.
	 */
	fPtr->linear = crime_mmap(CRIME_LINEAR_SIZE, fPtr->linear_phys,
	    fd, 0);
	if (fPtr->linear == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "mmap staging buffer: %s\n", strerror(errno));
		return FALSE;
	}

	fPtr->table = crime_mmap(CRIME_TILE_SIZE, fPtr->table_phys, fd, 0);
	if (fPtr->table == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "mmap GBE tile table: %s\n", strerror(errno));
		return FALSE;
	}
#endif
	return TRUE;
}

#ifndef CRIME_WSCONS
static Bool
crime_wait_dma_idle(CrimePtr fPtr)
{
	int bail = 100000;

	while (((GBE_READ(fPtr, CRMFB_OVR_CONTROL) & 1) ||
		(GBE_READ(fPtr, CRMFB_FRM_CONTROL) & 1) ||
		(GBE_READ(fPtr, CRMFB_DID_CONTROL) & 1)) && bail > 0) {
		usleep(10);
		bail--;
	}
	return bail > 0;
}

/* Stop the scanout DMA and the dot clock so the FRM setup can be changed */
static void
crime_gbe_stop(ScrnInfoPtr pScrn, CrimePtr fPtr)
{
	uint32_t d;
	int bail;

	GBE_WRITE(fPtr, CRMFB_OVR_CONTROL,
	    GBE_READ(fPtr, CRMFB_OVR_CONTROL) &
	    ~(1 << CRMFB_OVR_CONTROL_DMAEN_SHIFT));
	usleep(50000);
	GBE_WRITE(fPtr, CRMFB_FRM_CONTROL,
	    GBE_READ(fPtr, CRMFB_FRM_CONTROL) &
	    ~(1 << CRMFB_FRM_CONTROL_DMAEN_SHIFT));
	usleep(50000);
	GBE_WRITE(fPtr, CRMFB_DID_CONTROL, 0);
	usleep(50000);

	if (!crime_wait_dma_idle(fPtr))
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "timeout waiting for scanout DMA to stop\n");

	/* restart drawing at the top left once DMA is back on */
	GBE_WRITE(fPtr, CRMFB_VT_XY, 1 << CRMFB_VT_XY_FREEZE_SHIFT);
	usleep(1000);
	GBE_WRITE(fPtr, CRMFB_DOTCLOCK,
	    GBE_READ(fPtr, CRMFB_DOTCLOCK) &
	    ~(1 << CRMFB_DOTCLOCK_CLKRUN_SHIFT));
	bail = 10000;
	while ((GBE_READ(fPtr, CRMFB_DOTCLOCK) &
	    (1 << CRMFB_DOTCLOCK_CLKRUN_SHIFT)) && bail > 0) {
		usleep(10);
		bail--;
	}

	/* reset the scanout FIFO */
	d = GBE_READ(fPtr, CRMFB_FRM_TILESIZE);
	GBE_WRITE(fPtr, CRMFB_FRM_TILESIZE,
	    d | (1 << CRMFB_FRM_TILESIZE_FIFOR_SHIFT));
	GBE_WRITE(fPtr, CRMFB_FRM_TILESIZE,
	    d & ~(1 << CRMFB_FRM_TILESIZE_FIFOR_SHIFT));
}

static void
crime_gbe_start(CrimePtr fPtr)
{
	GBE_WRITE(fPtr, CRMFB_FRM_CONTROL,
	    GBE_READ(fPtr, CRMFB_FRM_CONTROL) |
	    (1 << CRMFB_FRM_CONTROL_DMAEN_SHIFT));
	GBE_WRITE(fPtr, CRMFB_VT_XY, 0);
	GBE_WRITE(fPtr, CRMFB_DOTCLOCK,
	    GBE_READ(fPtr, CRMFB_DOTCLOCK) |
	    (1 << CRMFB_DOTCLOCK_CLKRUN_SHIFT));
}

/*
 * Switch the GBE scanout from gbefb's linear one-tile-wide layout to the
 * native tiled layout the rendering engine works in, and point the CRIME
 * engine TLBs at the same memory.  This is what the NetBSD crmfb kernel
 * driver does at attach time; on Linux nobody else does it for us, and
 * touching the engine apertures with unprogrammed TLBs raises a hardware
 * bus error (SIGBUS).
 */
static void
crime_engine_setup(ScrnInfoPtr pScrn, CrimePtr fPtr)
{
	volatile uint64_t *tlb;
	volatile uint32_t *e32;
	uint64_t reg;
	uint32_t d, page;
	uint16_t v;
	int i, j, k, col;

	/* the engine TLB A: 16 rows of 16 tiles, 4 entries per register */
	v = (fPtr->fb_phys >> 16) & 0xffff;
	tlb = (volatile uint64_t *)(fPtr->engine + CRIME_RE_TLB_A);
	for (i = 0; i < 16; i++) {
		for (j = 0; j < 4; j++) {
			reg = 0;
			for (k = 0; k < 4; k++) {
				col = (j << 2) | k;
				if (i < fPtr->tile_rows &&
				    col < fPtr->tiles_x)
					reg |= (uint64_t)((v + i *
					    fPtr->tiles_x + col) | 0x8000)
					    << ((3 - k) << 4);
			}
			tlb[(i << 2) | j] = reg;
		}
	}

	/* the staging buffer goes into the first linear TLB, 4kB pages */
	page = (fPtr->linear_phys >> 12) | 0x80000000;
	tlb = (volatile uint64_t *)(fPtr->engine + CRIME_RE_LINEAR_A);
	for (i = 0; i < 16; i++) {
		tlb[i] = ((uint64_t)page << 32) | (page + 1);
		page += 2;
	}
	WBFLUSH;

	/* basic engine state */
	e32 = (volatile uint32_t *)fPtr->engine;
	e32[CRIME_DE_CLIPMODE >> 2] = 0;
	e32[CRIME_DE_WINOFFSET_SRC >> 2] = 0;
	e32[CRIME_DE_WINOFFSET_DST >> 2] = 0;
	e32[CRIME_DE_PLANEMASK >> 2] = 0xffffffff;
	tlb = (volatile uint64_t *)fPtr->engine;
	for (i = 0x20; i <= 0x40; i += 8)
		tlb[i >> 3] = 0;
	WBFLUSH;

	/*
	 * GBE tile table: the tiles of the current mode's viewport, in
	 * row major order.  The tiled framebuffer is laid out for the
	 * virtual screen, so rows are tiles_x tiles apart even when the
	 * mode is narrower.
	 */
	col = (fPtr->mode_width + 127) >> 7;
	k = 0;
	for (i = 0; i < (fPtr->mode_height + 127) >> 7; i++)
		for (j = 0; j < col; j++)
			fPtr->table[k++] = v + i * fPtr->tiles_x + j;
	/* park any overfetch on a valid tile */
	while (k < 256)
		fPtr->table[k++] = v;
	WBFLUSH;

	if (!fPtr->gbe_saved) {
		fPtr->saved_frm_control = GBE_READ(fPtr, CRMFB_FRM_CONTROL);
		fPtr->saved_frm_tilesize = GBE_READ(fPtr, CRMFB_FRM_TILESIZE);
		fPtr->saved_frm_pixsize = GBE_READ(fPtr, CRMFB_FRM_PIXSIZE);
		fPtr->gbe_saved = TRUE;
	}

	crime_gbe_stop(pScrn, fPtr);

	GBE_WRITE(fPtr, CRMFB_FRM_CONTROL,
	    (fPtr->table_phys >> 9) << CRMFB_FRM_CONTROL_TILEPTR_SHIFT);

	d = (fPtr->mode_width >> 7) << CRMFB_FRM_TILESIZE_WIDTH_SHIFT;
	d |= (((fPtr->mode_width & 127) * 4) >> 5) & 0x1f;
	d |= CRMFB_FRM_TILESIZE_DEPTH_32 << CRMFB_FRM_TILESIZE_DEPTH_SHIFT;
	GBE_WRITE(fPtr, CRMFB_FRM_TILESIZE, d);
	GBE_WRITE(fPtr, CRMFB_FRM_PIXSIZE,
	    fPtr->mode_height << CRMFB_FRM_PIXSIZE_HEIGHT_SHIFT);

	crime_gbe_start(fPtr);
}

/* Hand the scanout back to the layout gbefb set up, for fbcon */
static void
crime_engine_release(ScrnInfoPtr pScrn, CrimePtr fPtr)
{
	if (!fPtr->gbe_saved)
		return;

	crime_gbe_stop(pScrn, fPtr);
	GBE_WRITE(fPtr, CRMFB_FRM_TILESIZE, fPtr->saved_frm_tilesize);
	GBE_WRITE(fPtr, CRMFB_FRM_PIXSIZE, fPtr->saved_frm_pixsize);
	GBE_WRITE(fPtr, CRMFB_FRM_CONTROL,
	    fPtr->saved_frm_control &
	    ~(1 << CRMFB_FRM_CONTROL_DMAEN_SHIFT));
	crime_gbe_start(fPtr);
}

/*
 * Set a video mode: have gbefb program the display timings for it,
 * then take the scanout over again in tiled layout.  Requires the
 * engine/GBE mappings, so this only runs from ScreenInit onwards.
 */
static Bool
crime_set_mode(ScrnInfoPtr pScrn, DisplayModePtr mode)
{
	CrimePtr fPtr = CRIMEPTR(pScrn);
	struct fb_var_screeninfo var;

	crime_mode_to_var(fPtr, mode, &var);
	var.activate = FB_ACTIVATE_NOW;
	if (ioctl(fPtr->fd, FBIOPUT_VSCREENINFO, &var) == -1) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "cannot set fb mode %dx%d: %s\n",
			   mode->HDisplay, mode->VDisplay, strerror(errno));
		return FALSE;
	}
	fPtr->var = var;
	fPtr->mode_width = mode->HDisplay;
	fPtr->mode_height = mode->VDisplay;

	/*
	 * gbefb has just reprogrammed the whole display backend; save
	 * that state anew for the console before taking over.
	 */
	fPtr->gbe_saved = FALSE;
	crime_engine_setup(pScrn, fPtr);
	return TRUE;
}
#endif /* !CRIME_WSCONS */

static void
crime_unmap_engine(ScrnInfoPtr pScrn, CrimePtr fPtr)
{
	if (fPtr->engine && munmap(fPtr->engine, CRIME_ENGINE_SIZE) == -1)
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "munmap engine: %s\n", strerror(errno));
	if (fPtr->linear && munmap(fPtr->linear, CRIME_LINEAR_SIZE) == -1)
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "munmap linear: %s\n", strerror(errno));
	fPtr->engine = NULL;
	fPtr->linear = NULL;
#ifndef CRIME_WSCONS
	if (fPtr->gbe &&
	    munmap((void *)fPtr->gbe, CRIME_GBE_SIZE) == -1)
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "munmap GBE: %s\n", strerror(errno));
	fPtr->gbe = NULL;
	if (fPtr->table &&
	    munmap((void *)fPtr->table, CRIME_TILE_SIZE) == -1)
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "munmap tile table: %s\n", strerror(errno));
	fPtr->table = NULL;
	if (fPtr->memfd != -1) {
		close(fPtr->memfd);
		fPtr->memfd = -1;
	}
#endif
}

static Bool
CrimeProbe(DriverPtr drv, int flags)
{
	ScrnInfoPtr pScrn = NULL;
	int i, fd, entity;
	GDevPtr *devSections;
	int numDevSections;
	Bool foundScreen = FALSE;

	if ((numDevSections = xf86MatchDevice(CRIME_DRIVER_NAME,
					      &devSections)) <= 0)
		return FALSE;

	if ((fd = crime_open(CRIME_DEFAULT_DEV)) == -1) {
		free(devSections);
		return FALSE;
	}

	if (!crime_check_device(fd)) {
		close(fd);
		free(devSections);
		return FALSE;
	}
	close(fd);

	xf86Msg(X_INFO, "%s: CRIME found\n", __func__);

	if (flags & PROBE_DETECT) {
		free(devSections);
		return TRUE;
	}

	if (numDevSections > 1) {
		xf86Msg(X_ERROR, "Ignoring additional device sections\n");
		numDevSections = 1;
	}
	/* ok, at this point we know we've got a CRIME */
	for (i = 0; i < numDevSections; i++) {

		entity = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);
		pScrn = xf86ConfigFbEntity(NULL, 0, entity,
		    NULL, NULL, NULL, NULL);
		if (pScrn != NULL) {
			foundScreen = TRUE;
			pScrn->driverVersion = CRIME_VERSION;
			pScrn->driverName = CRIME_DRIVER_NAME;
			pScrn->name = CRIME_NAME;
			pScrn->Probe = CrimeProbe;
			pScrn->PreInit = CrimePreInit;
			pScrn->ScreenInit = CrimeScreenInit;
			pScrn->SwitchMode = CrimeSwitchMode;
#ifndef CRIME_WSCONS
			pScrn->ValidMode = CrimeValidMode;
#endif
			pScrn->AdjustFrame = NULL;
			pScrn->EnterVT = CrimeEnterVT;
			pScrn->LeaveVT = CrimeLeaveVT;
		}
	}
	free(devSections);
	return foundScreen;
}

static Bool
CrimePreInit(ScrnInfoPtr pScrn, int flags)
{
	CrimePtr fPtr;
	int default_depth;
	const char *dev;
	Gamma zeros = {0.0, 0.0, 0.0};
#ifdef CRIME_WSCONS
	DisplayModePtr mode;
#endif
	MessageType from;
	rgb rgbzeros = { 0, 0, 0 }, masks;

	if (flags & PROBE_DETECT) return FALSE;

	if (pScrn->numEntities != 1) return FALSE;

	pScrn->monitor = pScrn->confScreen->monitor;

	CrimeGetRec(pScrn);
	fPtr = CRIMEPTR(pScrn);
#ifndef CRIME_WSCONS
	fPtr->memfd = -1;
#endif

	fPtr->pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

	dev = xf86FindOptionValue(fPtr->pEnt->device->options, "device");
	fPtr->fd = crime_open(dev);
	if (fPtr->fd == -1) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "cannot open framebuffer device\n");
		return FALSE;
	}

	if (!crime_check_device(fPtr->fd)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "device is not a CRIME/GBE framebuffer\n");
		return FALSE;
	}

	if (!crime_get_fbinfo(pScrn, fPtr)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "cannot query framebuffer characteristics: %s\n",
			   strerror(errno));
		return FALSE;
	}

	/* Handle depth */
	default_depth = fPtr->info.depth <= 24 ? fPtr->info.depth : 24;
	if (!xf86SetDepthBpp(pScrn, default_depth, default_depth,
			     fPtr->info.depth, Support24bppFb|Support32bppFb))
		return FALSE;
	xf86PrintDepthBpp(pScrn);

	/* Get the depth24 pixmap format */
	if (pScrn->depth == 24 && pix24bpp == 0)
		pix24bpp = xf86GetBppFromDepth(pScrn, 24);

	/* color weight */
	masks.red =   0x00ff0000;
	masks.green = 0x0000ff00;
	masks.blue =  0x000000ff;
	if (!xf86SetWeight(pScrn, rgbzeros, masks))
		return FALSE;

	/* visual init */
	if (!xf86SetDefaultVisual(pScrn, -1))
		return FALSE;

	/* We don't currently support DirectColor at > 8bpp */
	if (pScrn->depth > 8 && pScrn->defaultVisual != TrueColor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Given default visual"
			   " (%s) is not supported at depth %d\n",
			   xf86GetVisualName(pScrn->defaultVisual),
			   pScrn->depth);
		return FALSE;
	}

	xf86SetGamma(pScrn,zeros);

	pScrn->progClock = TRUE;
	pScrn->rgbBits   = 8;
	pScrn->chipset   = "crime";

	/* handle options */
	xf86CollectOptions(pScrn, NULL);
	if (!(fPtr->Options = malloc(sizeof(CrimeOptions))))
		return FALSE;
	memcpy(fPtr->Options, CrimeOptions, sizeof(CrimeOptions));
	xf86ProcessOptions(pScrn->scrnIndex, fPtr->pEnt->device->options,
			   fPtr->Options);

#ifndef CRIME_WSCONS
	/*
	 * Build the mode list: configured modes validated against gbefb,
	 * with the mode detected on /dev/fb0 as fallback.  The tiled
	 * framebuffer layout covers the virtual (largest mode) area.
	 */
	if (!crime_select_modes(pScrn, fPtr))
		return FALSE;

	fPtr->info.width = pScrn->virtualX;
	fPtr->info.height = pScrn->virtualY;
	if (!crime_compute_layout(pScrn, fPtr))
		return FALSE;
	fPtr->vram_lines = fPtr->tile_rows * 128;

	/*
	 * We cannot pan, so the X screen must start at the initial
	 * mode's size, not the layout's.  The RandR shim resizes the
	 * screen on every mode switch (up to the layout maximum kept
	 * in displayWidth/info.width/height above).
	 */
	pScrn->virtualX = pScrn->currentMode->HDisplay;
	pScrn->virtualY = pScrn->currentMode->VDisplay;
#else
	fPtr->vram_lines = 2048;

	/* fake video mode struct */
	mode = (DisplayModePtr)malloc(sizeof(DisplayModeRec));
	mode->prev = mode;
	mode->next = mode;
	mode->name = "crime current mode";
	mode->status = MODE_OK;
	mode->type = M_T_BUILTIN;
	mode->Clock = 0;
	mode->HDisplay = fPtr->info.width;
	mode->HSyncStart = 0;
	mode->HSyncEnd = 0;
	mode->HTotal = 0;
	mode->HSkew = 0;
	mode->VDisplay = fPtr->info.height;
	mode->VSyncStart = 0;
	mode->VSyncEnd = 0;
	mode->VTotal = 0;
	mode->VScan = 0;
	mode->Flags = 0;
	if (pScrn->modes != NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Ignoring mode specification from screen section\n");
	}
	pScrn->currentMode = pScrn->modes = mode;
	pScrn->virtualX = fPtr->info.width;
	pScrn->virtualY = fPtr->info.height;
	pScrn->displayWidth = pScrn->virtualX;
#endif

	/* the tiled framebuffer actually used, in kB */
	pScrn->videoRam = fPtr->info.width * 4 * fPtr->vram_lines / 1024;
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Vidmem: %dk\n",
		   pScrn->videoRam);

	/* Set the display resolution */
	xf86SetDpi(pScrn, 0, 0);

	from = X_DEFAULT;
	fPtr->HWCursor = TRUE;
	if (xf86GetOptValBool(fPtr->Options, OPTION_HW_CURSOR, &fPtr->HWCursor))
		from = X_CONFIG;
	if (xf86ReturnOptValBool(fPtr->Options, OPTION_SW_CURSOR, FALSE)) {
		from = X_CONFIG;
		fPtr->HWCursor = FALSE;
	}
	xf86DrvMsg(pScrn->scrnIndex, from, "Using %s cursor\n",
		fPtr->HWCursor ? "HW" : "SW");

	if (xf86LoadSubModule(pScrn, "fb") == NULL) {
		CrimeFreeRec(pScrn);
		return FALSE;
	}

	if (xf86LoadSubModule(pScrn, "xaa") == NULL) {
		CrimeFreeRec(pScrn);
		return FALSE;
	}

	if (xf86LoadSubModule(pScrn, "ramdac") == NULL) {
		CrimeFreeRec(pScrn);
		return FALSE;
	}

	return TRUE;
}

static Bool
CrimeScreenInit(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	CrimePtr fPtr = CRIMEPTR(pScrn);
	VisualPtr visual;
	int ret, flags, width, height;

#ifdef CRIME_DEBUG
	ErrorF("\tbitsPerPixel=%d, depth=%d, defaultVisual=%s\n"
	       "\tmask: %x,%x,%x, offset: %d,%d,%d\n",
	       pScrn->bitsPerPixel,
	       pScrn->depth,
	       xf86GetVisualName(pScrn->defaultVisual),
	       pScrn->mask.red,pScrn->mask.green,pScrn->mask.blue,
	       pScrn->offset.red,pScrn->offset.green,pScrn->offset.blue);
#endif

	/* Switch to graphics mode - required before mmap on wscons */
	if (!crime_set_gfx_mode(fPtr, TRUE)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "cannot switch to graphics mode: %s\n",
			   strerror(errno));
		return FALSE;
	}

	if (!crime_map_engine(pScrn, fPtr))
		return FALSE;

#ifndef CRIME_WSCONS
	/*
	 * On server regeneration the RandR shim resets currentMode to
	 * the first mode but restores the initial virtual size; the
	 * screen is created at virtualX/Y, so resync currentMode to it.
	 */
	{
		DisplayModePtr m = pScrn->modes;
		do {
			if (m->HDisplay == pScrn->virtualX &&
			    m->VDisplay == pScrn->virtualY) {
				pScrn->currentMode = m;
				break;
			}
			m = m->next;
		} while (m != NULL && m != pScrn->modes);
	}

	/*
	 * Set the initial video mode; this also programs the engine
	 * TLBs before anything touches the apertures.
	 */
	if (!crime_set_mode(pScrn, pScrn->currentMode))
		return FALSE;
#endif

	memset(fPtr->linear, 0, CRIME_LINEAR_SIZE);

	/*
	 * The visible framebuffer is tiled and not usefully CPU
	 * addressable; all rendering goes through the engine.  fb only
	 * ever draws into this shadow, whose content is transferred by
	 * the XAA ImageWrite/ReadPixmap hooks.
	 */
	/* sized for the layout maximum - RandR may grow the screen */
	fPtr->fb = malloc(pScrn->displayWidth * 4 * fPtr->info.height);
	if (fPtr->fb == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Cannot allocate shadow fb: %s\n", strerror(errno));
		return FALSE;
	}
	/* rows beyond the initial mode become visible on a RandR grow */
	memset(fPtr->fb, 0, pScrn->displayWidth * 4 * fPtr->info.height);

	pScrn->vtSema = TRUE;

	/* mi layer */
	miClearVisualTypes();
	if (!miSetVisualTypes(pScrn->depth, TrueColorMask,
			      pScrn->rgbBits, TrueColor))
		return FALSE;

	if (!miSetPixmapDepths())
		return FALSE;

	height = pScrn->virtualY;
	width = pScrn->virtualX;

	ret = fbScreenInit(pScreen,
			   fPtr->fb,
			   width, height,
			   pScrn->xDpi, pScrn->yDpi,
			   pScrn->displayWidth,
			   pScrn->bitsPerPixel);

	if (!ret)
		return FALSE;

	/* Fixup RGB ordering */
	visual = pScreen->visuals + pScreen->numVisuals;
	while (--visual >= pScreen->visuals) {
		if ((visual->class | DynamicClass) == DirectColor) {
			visual->offsetRed   = pScrn->offset.red;
			visual->offsetGreen = pScrn->offset.green;
			visual->offsetBlue  = pScrn->offset.blue;
			visual->redMask     = pScrn->mask.red;
			visual->greenMask   = pScrn->mask.green;
			visual->blueMask    = pScrn->mask.blue;
		}
	}

	if (!fbPictureInit(pScreen, NULL, 0))
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "RENDER extension initialisation failed.");

	xf86SetBlackWhitePixels(pScreen);
	xf86SetBackingStore(pScreen);

	{
		BoxRec bx;
		fPtr->pXAA = XAACreateInfoRec();
		CrimeAccelInit(pScrn);
		/*
		 * Offscreen pixmap area: strictly below the largest
		 * mode, so a RandR screen grow never exposes it.  The
		 * area does not touch the (smaller) initial screen, so
		 * it is handed to the manager as a region directly.
		 */
		RegionRec offreg;

		bx.x1 = 0;
		bx.y1 = fPtr->info.height;
		bx.x2 = pScrn->displayWidth;
		bx.y2 = fPtr->vram_lines;
		if (bx.y2 < bx.y1)
			bx.y2 = bx.y1;	/* no offscreen space */
		RegionInit(&offreg, &bx, 1);
		xf86InitFBManagerRegion(pScreen, &offreg);
		RegionUninit(&offreg);
		if(!XAAInit(pScreen, fPtr->pXAA))
			return FALSE;
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Using acceleration\n");
	}

	/* software cursor */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	/* check for hardware cursor support */
	if (fPtr->HWCursor)
		CrimeSetupCursor(pScreen);

	/* colormap */
	if (!miCreateDefColormap(pScreen))
		return FALSE;
	flags = CMAP_RELOAD_ON_MODE_SWITCH;
	if(!xf86HandleColormaps(pScreen, 256, 8, CrimeLoadPalette,
				NULL, flags))
		return FALSE;

	pScreen->SaveScreen = CrimeSaveScreen;

#ifdef XvExtension
	{
		XF86VideoAdaptorPtr *ptr;

		int n = xf86XVListGenericAdaptors(pScrn,&ptr);
		if (n) {
			xf86XVScreenInit(pScreen,ptr,n);
		}
	}
#endif

	/* Wrap the current CloseScreen function */
	fPtr->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = CrimeCloseScreen;

	return TRUE;
}

static Bool
CrimeCloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	CrimePtr fPtr = CRIMEPTR(pScrn);

	if (pScrn->vtSema) {
		CrimeRestore(pScrn);
		crime_unmap_engine(pScrn, fPtr);
		free(fPtr->fb);
		fPtr->fb = NULL;
	}
	pScrn->vtSema = FALSE;

	/* unwrap CloseScreen */
	pScreen->CloseScreen = fPtr->CloseScreen;
	return (*pScreen->CloseScreen)(CLOSE_SCREEN_ARGS);
}

static Bool
CrimeEnterVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	pScreenInfo->vtSema = TRUE;
	crime_set_gfx_mode(CRIMEPTR(pScreenInfo), TRUE);
#ifndef CRIME_WSCONS
	/* re-set our mode (fbcon may have changed it) and retake the
	   scanout */
	if (!crime_set_mode(pScreenInfo, pScreenInfo->currentMode))
		return FALSE;
#endif
	return TRUE;
}

static void
CrimeLeaveVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	CrimeRestore(pScreenInfo);
}

static Bool
CrimeSwitchMode(SWITCH_MODE_ARGS_DECL)
{
#ifdef CRIME_WSCONS
	/* Nothing else to do */
	return TRUE;
#else
	SCRN_INFO_PTR(arg);

	return crime_set_mode(pScreenInfo, pMode);
#endif
}

static void
CrimeLoadPalette(ScrnInfoPtr pScrn, int numColors, int *indices,
	       LOCO *colors, VisualPtr pVisual)
{
	CrimePtr fPtr = CRIMEPTR(pScrn);
#ifdef CRIME_WSCONS
	struct wsdisplay_cmap cmap;
	unsigned char red[256],green[256],blue[256];
	int i, indexMin=256, indexMax=0;

	cmap.count   = 1;
	cmap.red   = red;
	cmap.green = green;
	cmap.blue  = blue;

	if (numColors == 1) {
		/* Optimisation */
		cmap.index = indices[0];
		red[0]   = colors[indices[0]].red;
		green[0] = colors[indices[0]].green;
		blue[0]  = colors[indices[0]].blue;
		if (ioctl(fPtr->fd,WSDISPLAYIO_PUTCMAP, &cmap) == -1)
			ErrorF("ioctl WSDISPLAYIO_PUTCMAP: %s\n",
			    strerror(errno));
	} else {
		/* Change all colors in 2 syscalls */
		/* and limit the data to be transfered */
		for (i = 0; i < numColors; i++) {
			if (indices[i] < indexMin)
				indexMin = indices[i];
			if (indices[i] > indexMax)
				indexMax = indices[i];
		}
		cmap.index = indexMin;
		cmap.count = indexMax - indexMin + 1;
		cmap.red = &red[indexMin];
		cmap.green = &green[indexMin];
		cmap.blue = &blue[indexMin];
		/* Get current map */
		if (ioctl(fPtr->fd, WSDISPLAYIO_GETCMAP, &cmap) == -1)
			ErrorF("ioctl WSDISPLAYIO_GETCMAP: %s\n",
			    strerror(errno));
		/* Change the colors that require updating */
		for (i = 0; i < numColors; i++) {
			red[indices[i]]   = colors[indices[i]].red;
			green[indices[i]] = colors[indices[i]].green;
			blue[indices[i]]  = colors[indices[i]].blue;
		}
		/* Write the colormap back */
		if (ioctl(fPtr->fd,WSDISPLAYIO_PUTCMAP, &cmap) == -1)
			ErrorF("ioctl WSDISPLAYIO_PUTCMAP: %s\n",
			    strerror(errno));
	}
#else
	struct fb_cmap cmap;
	uint16_t red, green, blue;
	int i;

	for (i = 0; i < numColors; i++) {
		int idx = indices[i];

		red   = colors[idx].red << 8;
		green = colors[idx].green << 8;
		blue  = colors[idx].blue << 8;
		cmap.start = idx;
		cmap.len = 1;
		cmap.red = &red;
		cmap.green = &green;
		cmap.blue = &blue;
		cmap.transp = NULL;
		if (ioctl(fPtr->fd, FBIOPUTCMAP, &cmap) == -1)
			ErrorF("ioctl FBIOPUTCMAP: %s\n", strerror(errno));
	}
#endif
}

static Bool
CrimeSaveScreen(ScreenPtr pScreen, int mode)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	CrimePtr fPtr = CRIMEPTR(pScrn);
	int state;

	if (!pScrn->vtSema)
		return TRUE;

	if (mode != SCREEN_SAVER_FORCER) {
#ifdef CRIME_WSCONS
		state = xf86IsUnblank(mode)?WSDISPLAYIO_VIDEO_ON:
		                            WSDISPLAYIO_VIDEO_OFF;
		ioctl(fPtr->fd, WSDISPLAYIO_SVIDEO, &state);
#else
		state = xf86IsUnblank(mode) ? FB_BLANK_UNBLANK :
					      FB_BLANK_NORMAL;
		ioctl(fPtr->fd, FBIOBLANK, state);
#endif
	}
	return TRUE;
}

static void
CrimeRestore(ScrnInfoPtr pScrn)
{
	CrimePtr fPtr = CRIMEPTR(pScrn);

#ifndef CRIME_WSCONS
	/* the console knows nothing about our hardware cursor - hide it */
	if (fPtr->gbe)
		fPtr->gbe[CRMFB_CURSOR_CONTROL >> 2] = 0;

	/* give the scanout back to fbcon's linear layout */
	crime_engine_release(pScrn, fPtr);
#endif

	/* Restore the text mode */
	if (!crime_set_gfx_mode(fPtr, FALSE)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "error setting text mode %s\n", strerror(errno));
	}
}
