/* $NetBSD: crime_cursor.c,v 1.3 2011/05/20 01:49:48 christos Exp $ */
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

/*
 * Based on fbdev.c written by:
 *
 * Authors:  Alan Hourihane, <alanh@fairlite.demon.co.uk>
 *	     Michel Dänzer, <michdaen@iiic.ethz.ch>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <errno.h>

/* all driver need this */
#include "xf86.h"
#include "xf86_OSproc.h"

#include "crime.h"

#ifndef CRIME_WSCONS
/*
 * Direct GBE hardware cursor.  The Linux gbefb driver offers no cursor
 * interface, so the CRMFB cursor registers (mapped from /dev/mem along
 * with the engine) are programmed directly, mirroring what the NetBSD
 * crmfb kernel driver does for WSDISPLAYIO_SCURSOR.
 *
 * The cursor is 32x32 pixels, 2 bits each: 0 is transparent, non-zero
 * values index the three cursor colormap registers.  A source bit and
 * its mask bit form one pixel, so a foreground pixel (source & mask)
 * reads as 3 -> CMAP2 and a background pixel (mask only) as 1 or 2
 * depending on scan direction; both CMAP0 and CMAP1 are set to the
 * background color so either reading shows the right thing.
 */

#define CRIME_CURSOR_SIZE	32
#define CRIME_CURSOR_MASKOFF	((CRIME_CURSOR_SIZE >> 3) * CRIME_CURSOR_SIZE)

#define GBE_WRITE(fPtr, reg, val) \
	((fPtr)->gbe[(reg) >> 2] = (val))

static void
CrimeShowCursor(ScrnInfoPtr pScrn)
{
	CrimePtr pCrime = CRIMEPTR(pScrn);

	GBE_WRITE(pCrime, CRMFB_CURSOR_CONTROL, CRMFB_CURSOR_ON);
}

static void
CrimeHideCursor(ScrnInfoPtr pScrn)
{
	CrimePtr pCrime = CRIMEPTR(pScrn);

	GBE_WRITE(pCrime, CRMFB_CURSOR_CONTROL, 0);
}

static void
CrimeSetCursorPosition(ScrnInfoPtr pScrn, int x, int y)
{
	CrimePtr pCrime = CRIMEPTR(pScrn);

	/* both fields are signed 16 bit, no hotspot register needed */
	GBE_WRITE(pCrime, CRMFB_CURSOR_POS,
	    ((uint32_t)(y & 0xffff) << 16) | (uint32_t)(x & 0xffff));
}

static void
CrimeSetCursorColors(ScrnInfoPtr pScrn, int bg, int fg)
{
	CrimePtr pCrime = CRIMEPTR(pScrn);
	uint32_t bgval, fgval;

	/* cursor colormap entries are R<<24 | G<<16 | B<<8, i.e. the
	   0x00RRGGBB value we get shifted up one byte */
	bgval = (uint32_t)(bg & 0xffffff) << 8;
	fgval = (uint32_t)(fg & 0xffffff) << 8;

	GBE_WRITE(pCrime, CRMFB_CURSOR_CMAP0, bgval);
	GBE_WRITE(pCrime, CRMFB_CURSOR_CMAP1, bgval);
	GBE_WRITE(pCrime, CRMFB_CURSOR_CMAP2, fgval);
}

static void
CrimeLoadCursorImage(ScrnInfoPtr pScrn, unsigned char *src)
{
	CrimePtr pCrime = CRIMEPTR(pScrn);
	unsigned char *mask = src + CRIME_CURSOR_MASKOFF;
	uint32_t latch, omask;
	uint8_t imask;
	int i, j, cnt = 0;

	/*
	 * Interleave the source and mask bitmaps into the 2bpp cursor
	 * bitmap, two source bytes (16 pixels) per 32bit word, exactly
	 * like the NetBSD crmfb kernel driver does.
	 */
	for (i = 0; i < (CRIME_CURSOR_SIZE * CRIME_CURSOR_SIZE) / 16; i++) {
		latch = 0;
		omask = 0x80000000;
		imask = 0x01;
		for (j = 0; j < 8; j++) {
			if (src[cnt] & imask)
				latch |= omask;
			omask >>= 1;
			if (mask[cnt] & imask)
				latch |= omask;
			omask >>= 1;
			imask <<= 1;
		}
		cnt++;
		imask = 0x01;
		for (j = 0; j < 8; j++) {
			if (src[cnt] & imask)
				latch |= omask;
			omask >>= 1;
			if (mask[cnt] & imask)
				latch |= omask;
			omask >>= 1;
			imask <<= 1;
		}
		cnt++;
		GBE_WRITE(pCrime, CRMFB_CURSOR_BITMAP + (i << 2), latch);
	}
}

Bool
CrimeSetupCursor(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	CrimePtr pCrime = CRIMEPTR(pScrn);
	xf86CursorInfoPtr infoPtr;

	if (pCrime->gbe == NULL) {
		xf86Msg(X_WARNING,
		    "GBE registers not mapped, using software cursor\n");
		return FALSE;
	}

	infoPtr = xf86CreateCursorInfoRec();
	if (!infoPtr)
		return FALSE;

	pCrime->CursorInfoRec = infoPtr;

	/* start out hidden */
	GBE_WRITE(pCrime, CRMFB_CURSOR_CONTROL, 0);

	xf86Msg(X_INFO, "HW cursor enabled\n");

	infoPtr->MaxWidth = CRIME_CURSOR_SIZE;
	infoPtr->MaxHeight = CRIME_CURSOR_SIZE;
	infoPtr->Flags = HARDWARE_CURSOR_AND_SOURCE_WITH_MASK |
	    HARDWARE_CURSOR_TRUECOLOR_AT_8BPP;

	infoPtr->SetCursorColors = CrimeSetCursorColors;
	infoPtr->SetCursorPosition = CrimeSetCursorPosition;
	infoPtr->LoadCursorImage = CrimeLoadCursorImage;
	infoPtr->HideCursor = CrimeHideCursor;
	infoPtr->ShowCursor = CrimeShowCursor;
	infoPtr->UseHWCursor = NULL;

	return xf86InitCursor(pScreen, infoPtr);
}
#else

static void CrimeLoadCursorImage(ScrnInfoPtr pScrn, unsigned char *src);
static void CrimeSetCursorPosition(ScrnInfoPtr pScrn, int x, int y);
static void CrimeSetCursorColors(ScrnInfoPtr pScrn, int bg, int fg);

static void
CrimeLoadCursorImage(ScrnInfoPtr pScrn, unsigned char *src)
{
	CrimePtr pCrime = CRIMEPTR(pScrn);
	int err, i;
	
	pCrime->cursor.which = WSDISPLAY_CURSOR_DOALL;
	pCrime->cursor.image = src;
	pCrime->cursor.mask = src + pCrime->maskoffset;
	if(ioctl(pCrime->fd, WSDISPLAYIO_SCURSOR, &pCrime->cursor) == -1)
		xf86Msg(X_ERROR, "CrimeLoadCursorImage: %d\n", errno);
}

void 
CrimeShowCursor(ScrnInfoPtr pScrn)
{
	CrimePtr pCrime = CRIMEPTR(pScrn);

	pCrime->cursor.which = WSDISPLAY_CURSOR_DOCUR;
	pCrime->cursor.enable = 1;
	if(ioctl(pCrime->fd, WSDISPLAYIO_SCURSOR, &pCrime->cursor) == -1)
		xf86Msg(X_ERROR, "CrimeShowCursor: %d\n", errno);
}

void
CrimeHideCursor(ScrnInfoPtr pScrn)
{
	CrimePtr pCrime = CRIMEPTR(pScrn);

	pCrime->cursor.which = WSDISPLAY_CURSOR_DOCUR;
	pCrime->cursor.enable = 0;
	if(ioctl(pCrime->fd, WSDISPLAYIO_SCURSOR, &pCrime->cursor) == -1)
		xf86Msg(X_ERROR, "CrimeHideCursor: %d\n", errno);
}

static void
CrimeSetCursorPosition(ScrnInfoPtr pScrn, int x, int y)
{
	CrimePtr pCrime = CRIMEPTR(pScrn);
	int xoff = 0, yoff = 0;
	
	pCrime->cursor.which = WSDISPLAY_CURSOR_DOPOS | WSDISPLAY_CURSOR_DOHOT;
	
	if (x < 0) {
		xoff = -x;
		x = 0;
	}
	if (y < 0) {
		yoff = -y;
		y = 0;
	}
	
	pCrime->cursor.pos.x = x;
	pCrime->cursor.hot.x = xoff;
	pCrime->cursor.pos.y = y;
	pCrime->cursor.hot.y = yoff;
	
	if(ioctl(pCrime->fd, WSDISPLAYIO_SCURSOR, &pCrime->cursor) == -1)
		xf86Msg(X_ERROR, "CrimeSetCursorPosition: %d\n", errno);
}

static void
CrimeSetCursorColors(ScrnInfoPtr pScrn, int bg, int fg)
{
	CrimePtr pCrime = CRIMEPTR(pScrn);
	u_char r[4], g[4], b[4];
	
	pCrime->cursor.which = WSDISPLAY_CURSOR_DOCMAP;
	pCrime->cursor.cmap.red = r;
	pCrime->cursor.cmap.green = g;
	pCrime->cursor.cmap.blue = b;
	r[1] = fg & 0xff;
	g[1] = (fg & 0xff00) >> 8;
	b[1] = (fg & 0xff0000) >> 16;
	r[0] = bg & 0xff;
	g[0] = (bg & 0xff00) >> 8;
	b[0] = (bg & 0xff0000) >> 16;
	pCrime->cursor.cmap.index = 0;
	pCrime->cursor.cmap.count = 2;
	if(ioctl(pCrime->fd, WSDISPLAYIO_SCURSOR, &pCrime->cursor) == -1)
		xf86Msg(X_ERROR, "CrimeSetCursorColors: %d\n", errno);
}

Bool
CrimeSetupCursor(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	CrimePtr pCrime = CRIMEPTR(pScrn);
	xf86CursorInfoPtr infoPtr;
	
	pCrime->cursor.pos.x = 0;
	pCrime->cursor.pos.y = 0;
	pCrime->cursor.enable = 0;

	infoPtr = xf86CreateCursorInfoRec();
	if(!infoPtr) return FALSE;
    
	pCrime->CursorInfoRec = infoPtr;
	if(ioctl(pCrime->fd, WSDISPLAYIO_GCURMAX, &pCrime->cursor.size) == -1) {
		xf86Msg(X_WARNING, "No HW cursor support found\n");
		return FALSE;
	}
		
	xf86Msg(X_INFO, "HW cursor enabled\n");

	infoPtr->MaxWidth = pCrime->cursor.size.x;
	infoPtr->MaxHeight = pCrime->cursor.size.y;
	pCrime->maskoffset = ( pCrime->cursor.size.x >> 3) * pCrime->cursor.size.y;
	
	pCrime->cursor.hot.x = 0;
	pCrime->cursor.hot.y = 0;
	pCrime->cursor.which = WSDISPLAY_CURSOR_DOHOT | WSDISPLAY_CURSOR_DOCUR |
	    WSDISPLAY_CURSOR_DOPOS;
	if(ioctl(pCrime->fd, WSDISPLAYIO_SCURSOR, &pCrime->cursor) == -1)
		xf86Msg(X_ERROR, "WSDISPLAYIO_SCURSOR: %d\n", errno);
	
	infoPtr->Flags = HARDWARE_CURSOR_AND_SOURCE_WITH_MASK |
	    HARDWARE_CURSOR_TRUECOLOR_AT_8BPP |
	    HARDWARE_CURSOR_BIT_ORDER_MSBFIRST;

	infoPtr->SetCursorColors = CrimeSetCursorColors;
	infoPtr->SetCursorPosition = CrimeSetCursorPosition;
	infoPtr->LoadCursorImage = CrimeLoadCursorImage;
	infoPtr->HideCursor = CrimeHideCursor;
	infoPtr->ShowCursor = CrimeShowCursor;
	infoPtr->UseHWCursor = NULL;

	return xf86InitCursor(pScreen, infoPtr);
}

#endif /* CRIME_WSCONS */
