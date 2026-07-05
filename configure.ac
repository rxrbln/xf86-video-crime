# Process this file with autoconf to produce a configure script

AC_PREREQ([2.60])
AC_INIT([xf86-video-crime],
        [0.9.0],
        [https://github.com/rxrbln/xf86-video-crime/issues],
        [xf86-video-crime])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR(.)

AM_INIT_AUTOMAKE([foreign dist-xz])

# Require X.Org macros 1.8 or later for MAN_SUBSTS set by XORG_MANPAGE_SECTIONS
m4_ifndef([XORG_MACROS_VERSION],
          [m4_fatal([must install xorg-macros 1.8 or later before running autoconf/autogen])])
XORG_MACROS_VERSION(1.8)
XORG_DEFAULT_OPTIONS

LT_INIT([disable-static])

AH_TOP([#include "xorg-server.h"])

# Define a configure option for an alternate module directory
AC_ARG_WITH(xorg-module-dir,
            AS_HELP_STRING([--with-xorg-module-dir=DIR],
                           [Default xorg module directory [[default=$libdir/xorg/modules]]]),
            [moduledir="$withval"],
            [moduledir="$libdir/xorg/modules"])

# Store the list of server defined optional extensions in REQUIRED_MODULES
XORG_DRIVER_CHECK_EXT(RANDR, randrproto)
XORG_DRIVER_CHECK_EXT(RENDER, renderproto)

# Obtain compiler/linker options for the driver dependencies
PKG_CHECK_MODULES(XORG, [xorg-server >= 1.18 xproto fontsproto $REQUIRED_MODULES])

sdkdir=`$PKG_CONFIG --variable=sdkdir xorg-server`

# The CRIME engine is only reachable through XAA-accelerated operations
# (the CPU-visible framebuffer is tiled), so XAA is a hard requirement.
save_CFLAGS=$CFLAGS
save_CPPFLAGS=$CPPFLAGS
CFLAGS=$XORG_CFLAGS
CPPFLAGS="$XORG_CFLAGS"
AC_CHECK_HEADERS([xaa.h],,
                 [AC_MSG_ERROR([xaa.h not found - this driver requires an X server with XAA support])])
CFLAGS=$save_CFLAGS
CPPFLAGS=$save_CPPFLAGS

# Backend selection: wscons (NetBSD/OpenBSD) or Linux fbdev + /dev/mem
AC_CHECK_HEADERS([dev/wscons/wsconsio.h], [have_wscons=yes], [have_wscons=no])
AC_CHECK_HEADERS([linux/fb.h], [have_linux_fb=yes], [have_linux_fb=no])
if test "x$have_wscons" = xno -a "x$have_linux_fb" = xno; then
    AC_MSG_ERROR([neither wscons nor Linux fbdev headers found])
fi

AC_SUBST([moduledir])

DRIVER_NAME=crime
AC_SUBST([DRIVER_NAME])

AC_CONFIG_FILES([
                Makefile
                src/Makefile
                man/Makefile
])
AC_OUTPUT
