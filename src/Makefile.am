# this is obnoxious:
# -module lets us name the module exactly how we want
# -avoid-version prevents gratuitous .0.0.0 version numbers on the end
# _ladir passes a dummy rpath to libtool so the thing will actually link

AM_CFLAGS = @XORG_CFLAGS@

crime_drv_la_LTLIBRARIES = crime_drv.la
crime_drv_la_LDFLAGS = -module -avoid-version
crime_drv_ladir = @moduledir@/drivers

crime_drv_la_SOURCES = \
	crime_driver.c \
	crime_cursor.c \
	crime_accel.c \
	crime.h \
	crmfbreg.h \
	compat-api.h
