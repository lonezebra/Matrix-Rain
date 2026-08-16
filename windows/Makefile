# Matrix Rain screensaver — MinGW (cross or native) build.
#   Linux/WSL cross build:  make            (uses x86_64-w64-mingw32-gcc)
#   Native MinGW build:     make CC=gcc

# make's built-in default CC=cc counts as "defined", so ?= alone would not
# pick the cross compiler; treat the built-in default as unset.
ifeq ($(origin CC),default)
CC = x86_64-w64-mingw32-gcc
endif
CC ?= x86_64-w64-mingw32-gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
LDFLAGS  = -mwindows -municode
LDLIBS   = -lgdi32 -luser32 -lcomctl32 -ladvapi32

TARGET = MatrixRain.scr
SRC    = matrix_rain.c rain.c

all: $(TARGET)

$(TARGET): $(SRC) rain.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
