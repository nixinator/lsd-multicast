# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2020 Brett Sheffield <bacs@librecast.net>

CFLAGS += -shared -fPIC
OBJS = lex.yy.o y.tab.o config.o log.o opts.o server.o wire.o $(PROGRAM).o

all: $(PROGRAM) keymgr

$(PROGRAM): $(OBJS)
	$(CC) $(OBJS) -o $@ -llibrecast -llsdb -ldl

$(PROGRAM).o:	$(PROGRAM).h

keymgr: keymgr.o
	$(CC) -o $@ $^ -lsodium

keymgr.o:

config.o: config.h lex.h

opts.o: opts.h

server.o: server.h

wire.o: wire.h

lex.yy.o:

y.tab.o: y.tab.h

y.tab.h: grammar.y
	$(YACC) -d $^

y.tab.c: grammar.y

lex.h: lex.yy.c
lex.yy.c: lexer.l y.tab.h
	$(LEX) --header-file=lex.h lexer.l

.PHONY: clean

clean:
	rm -f *.o $(PROGRAM) keymgr

realclean: clean
	rm -f y.tab.c y.tab.h lex.yy.c lex.h
