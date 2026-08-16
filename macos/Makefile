# Builds MatrixRain.saver with plain swiftc — no Xcode project required.
#
# ARCH=both (default) builds arm64 + x86_64 slices and lipos them into a
# universal binary, falling back gracefully to whichever slice builds.
# ARCH=arm64 or ARCH=x86_64 builds a single slice.
#
# swiftc -emit-library produces a dylib (MH_DYLIB); NSBundle loads bundle
# executables via dlopen, which accepts dylibs, so this works as the
# CFBundlePackageType BNDL executable.

MODULE     := MatrixRain
BUNDLE     := MatrixRain.saver
SOURCES    := $(wildcard Sources/*.swift)
BUILD_DIR  := build
BINARY     := $(BUILD_DIR)/$(MODULE)
ARCH       ?= both
MACOS_MIN  := 12

SWIFTFLAGS := -O -parse-as-library -emit-library -module-name $(MODULE) \
              -framework ScreenSaver -framework AppKit -framework QuartzCore

all: $(BUNDLE)

ifeq ($(ARCH),both)
$(BINARY): $(SOURCES)
	@mkdir -p $(BUILD_DIR)
	@built=""; \
	for arch in arm64 x86_64; do \
	  echo "swiftc [$$arch]"; \
	  if swiftc $(SWIFTFLAGS) -target $$arch-apple-macos$(MACOS_MIN) \
	       -o $(BUILD_DIR)/$(MODULE)-$$arch $(SOURCES); then \
	    built="$$built $(BUILD_DIR)/$(MODULE)-$$arch"; \
	  else \
	    echo "warning: $$arch slice failed, continuing" >&2; \
	  fi; \
	done; \
	set -- $$built; \
	if [ $$# -eq 0 ]; then \
	  echo "error: no architecture slice built" >&2; exit 1; \
	elif [ $$# -eq 1 ]; then \
	  cp "$$1" $(BINARY); \
	else \
	  lipo -create -output $(BINARY) $$built; \
	fi
else
$(BINARY): $(SOURCES)
	@mkdir -p $(BUILD_DIR)
	swiftc $(SWIFTFLAGS) -target $(ARCH)-apple-macos$(MACOS_MIN) -o $(BINARY) $(SOURCES)
endif

$(BUNDLE): $(BINARY) Info.plist
	rm -rf $(BUNDLE)
	mkdir -p $(BUNDLE)/Contents/MacOS
	cp Info.plist $(BUNDLE)/Contents/
	cp $(BINARY) $(BUNDLE)/Contents/MacOS/$(MODULE)
	codesign --force --sign - $(BUNDLE)

install: $(BUNDLE)
	mkdir -p "$(HOME)/Library/Screen Savers"
	rm -rf "$(HOME)/Library/Screen Savers/$(BUNDLE)"
	cp -R $(BUNDLE) "$(HOME)/Library/Screen Savers/"

clean:
	rm -rf $(BUILD_DIR) $(BUNDLE)

.PHONY: all install clean
