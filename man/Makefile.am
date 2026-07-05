#
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  on the rights to use, copy, modify, merge, publish, distribute, sub
#  license, and/or sell copies of the Software, and to permit persons to whom
#  the Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.

drivermandir = $(DRIVER_MAN_DIR)

driverman_PRE = crime.man

driverman_DATA = $(driverman_PRE:man=$(DRIVER_MAN_SUFFIX))

EXTRA_DIST = $(driverman_PRE)

CLEANFILES = $(driverman_DATA)

SUFFIXES = .$(DRIVER_MAN_SUFFIX) .man

# String replacements in MAN_SUBSTS now come from xorg-macros.m4 via configure
.man.$(DRIVER_MAN_SUFFIX):
	$(AM_V_GEN)$(SED) $(MAN_SUBSTS) < $< > $@
