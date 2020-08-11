# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2020 Brett Sheffield <bacs@librecast.net>

INSTALLDIR=/usr/local/bin

.PHONY: all clean src modules test check install

all: src

install: all
	cd src && $(MAKE) $@

src modules:
	cd $@ && $(MAKE)

clean realclean:
	cd src && $(MAKE) $@
	cd modules && $(MAKE) $@
	cd test && $(MAKE) $@

sparse: clean
	CC=cgcc $(MAKE) src

check test:
	cd test && $(MAKE) $@

%.test %.check:
	cd test && $(MAKE) -B $@
