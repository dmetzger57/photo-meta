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

## Install

Requires macOS 12 or later, on Apple silicon or Intel.

**Quickest:** paste this into Terminal. It downloads the latest release
into `/Applications`:

```sh
curl -fsSL https://raw.githubusercontent.com/dmetzger57/photo-meta/main/install.sh | bash
```

**Manual:** download `photo-meta.dmg` from the
[latest release](https://github.com/dmetzger57/photo-meta/releases/latest),
open it and drag **photo-meta** to Applications.

> The app isn't notarized by Apple, so macOS warns that it can't verify the
> developer the first time you open a manually downloaded copy. Go to
> **System Settings → Privacy & Security** and click **Open Anyway**, or run
> `xattr -dr com.apple.quarantine /Applications/photo-meta.app`.
> The install script doesn't need this step.

## Build from source

Requires the Xcode Command Line Tools (`xcode-select --install`).

```sh
make            # builds build/photo-meta.app (universal: Apple silicon + Intel)
make run        # build and launch
make install    # copy to /Applications
make dist       # release artifacts in build/dist (dmg, zip, SHA256SUMS)
make clean
```

To publish a release, bump `CFBundleShortVersionString` in
`resources/Info.plist`, then push a matching tag (`git tag v0.0.3 && git push --tags`).
GitHub Actions builds the app and attaches the dmg and zip to a new GitHub Release.

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
LICENSE           MIT License
install.sh        one-line installer for the latest GitHub release
.github/          release workflow (builds + publishes on v* tags)
```

## License

photo-meta is released under the [MIT License](LICENSE).
Copyright (c) 2026 Dennis Metzger.
