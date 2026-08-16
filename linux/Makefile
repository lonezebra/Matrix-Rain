PREFIX ?= /usr/local
PKGS    = x11 xft xext fontconfig

CC     ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS)) -lm

OBJS = matrix_rain.o rain.o

all: matrix-rain

matrix-rain: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

matrix_rain.o: matrix_rain.c rain.h
rain.o: rain.c rain.h

install: all
	install -Dm755 matrix-rain $(DESTDIR)$(PREFIX)/bin/matrix-rain
	install -Dm644 matrix-rain.desktop $(DESTDIR)$(PREFIX)/share/applications/matrix-rain.desktop

clean:
	rm -f matrix-rain $(OBJS)

.PHONY: all install clean
