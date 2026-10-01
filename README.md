# photo-meta

A small, native macOS app for looking at a photo's metadata. It is written in C.

Choose a photo with the file picker, or drop one onto the window. photo-meta shows:

- **A sidebar summary**: a preview, the file name, kind and size, then the
  dimensions, camera, lens, exposure (aperture, shutter, ISO, focal length),
  date taken and GPS location. If the photo has a location, there's a
  **Show in Maps** button.
- **A full metadata table**: every field macOS's ImageIO can read, in grouped
  sections. The sections are File, Image, Camera & Software (TIFF),
  Exposure & Capture (Exif), Lens & Body, Location (GPS), Description (IPTC),
  format-specific sections (HEIF, PNG, JFIF, GIF, DNG, RAW…), Container, XMP
  and vendor maker notes. Values are made readable: `1/120 s`, `ƒ/1.8`,
  `−0.3 EV`, `37.331820° N`, `Pattern (matrix)`, `★★★★☆` and so on.

It reads anything ImageIO supports: JPEG, HEIC/HEIF, PNG, TIFF, GIF, WebP,
camera RAW (CR2/CR3, NEF, ARW, DNG, RAF, ORF…), BMP, OpenEXR and more.

## Build & run

Requires the Xcode Command Line Tools (`xcode-select --install`) on macOS 12 or later.

```sh
make            # builds build/photo-meta.app (universal: Apple silicon + Intel)
make run        # build and launch
make install    # copy to /Applications
make clean
```

From the terminal:

```sh
build/photo-meta.app/Contents/MacOS/photo-meta IMG_1234.HEIC   # open a photo in the app
build/photo-meta --dump IMG_1234.HEIC                          # print all metadata as text
```

The app also registers as an alternate viewer for images, so it shows up in
Finder's **Open With** menu.

## Using it

| Action                         | How                                   |
| ------------------------------ | ------------------------------------- |
| Open a photo                   | ⌘O, the **Open Photo…** button, or drag & drop |
| Filter fields                  | ⌘F, then type (matches names, values and section titles) |
| Copy fields                    | Select rows and press ⌘C (with nothing selected, ⌘C copies everything) |
| Show the file in Finder        | ⇧⌘R                                   |

## How it's built

There is no Objective-C or Swift code. The UI is ordinary AppKit
(NSWindow, NSTableView, NSVisualEffectView, NSStackView, NSOpenPanel…),
driven from C through the Objective-C runtime (`objc_msgSend`,
`objc_allocateClassPair`). Metadata comes from Apple's C APIs in ImageIO and
CoreFoundation.

```
src/objc.h        typed objc_msgSend helpers
src/meta.c/.h     metadata extraction and formatting (no UI)
src/main.c        the AppKit UI, menus, drag & drop, CLI entry point
tools/make_icon.c renders the app icon with CoreGraphics at build time
resources/        Info.plist
```
