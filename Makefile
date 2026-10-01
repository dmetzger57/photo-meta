APP      := photo-meta
BUILD    := build
BUNDLE   := $(BUILD)/$(APP).app
BIN      := $(BUNDLE)/Contents/MacOS/$(APP)

CC       := clang
ARCHS    ?= -arch arm64 -arch x86_64
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -fblocks -mmacosx-version-min=12.0 $(ARCHS)
FRAMEWORKS := -framework Cocoa -framework ImageIO -framework CoreGraphics \
              -framework UniformTypeIdentifiers -framework CoreFoundation

SRC      := src/main.c src/meta.c
HDR      := src/meta.h src/objc.h

.PHONY: all run install clean

all: $(BUNDLE)

$(BUNDLE): $(BIN) $(BUNDLE)/Contents/Info.plist $(BUNDLE)/Contents/Resources/AppIcon.icns
	codesign --force --sign - $(BUNDLE) >/dev/null 2>&1 || true
	@touch $(BUNDLE)

$(BUILD)/$(APP): $(SRC) $(HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SRC) $(FRAMEWORKS) -o $@

$(BIN): $(BUILD)/$(APP)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUNDLE)/Contents/Info.plist: resources/Info.plist
	@mkdir -p $(dir $@)
	cp $< $@

$(BUNDLE)/Contents/Resources/AppIcon.icns: tools/make_icon.c
	@mkdir -p $(dir $@) $(BUILD)/AppIcon.iconset
	$(CC) -O2 -Wall tools/make_icon.c -framework CoreGraphics -framework ImageIO -framework CoreFoundation -o $(BUILD)/make_icon
	$(BUILD)/make_icon $(BUILD)/AppIcon.iconset
	iconutil -c icns $(BUILD)/AppIcon.iconset -o $@

run: all
	open $(BUNDLE)

install: all
	rm -rf /Applications/$(APP).app
	cp -R $(BUNDLE) /Applications/

clean:
	rm -rf $(BUILD)
