#!/bin/bash
# Install the latest photo-meta release into /Applications.
#
#   curl -fsSL https://raw.githubusercontent.com/dmetzger57/photo-meta/main/install.sh | bash
#
# Set PHOTO_META_DIR to install somewhere else (e.g. PHOTO_META_DIR=~/Applications).
#
# Copyright (c) 2026 Dennis Metzger
# SPDX-License-Identifier: MIT
set -euo pipefail

REPO="dmetzger57/photo-meta"
APP="photo-meta.app"
DEST="${PHOTO_META_DIR:-/Applications}"
URL="https://github.com/$REPO/releases/latest/download/photo-meta.zip"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "photo-meta is a macOS app; this installer only runs on macOS." >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading photo-meta…"
curl -fL --progress-bar "$URL" -o "$tmp/photo-meta.zip"
ditto -x -k "$tmp/photo-meta.zip" "$tmp"

if [[ ! -d "$tmp/$APP" ]]; then
    echo "The download did not contain $APP." >&2
    exit 1
fi

# Use sudo only if the destination isn't writable (e.g. a non-admin account).
SUDO=""
mkdir -p "$DEST" 2>/dev/null || true
if [[ ! -w "$DEST" ]]; then
    echo "Administrator access is needed to write to $DEST."
    SUDO="sudo"
fi

$SUDO rm -rf "$DEST/$APP"
$SUDO ditto "$tmp/$APP" "$DEST/$APP"
# The app is ad-hoc signed, not notarized; make sure no quarantine flag blocks it.
$SUDO xattr -dr com.apple.quarantine "$DEST/$APP" 2>/dev/null || true

version="$(defaults read "$DEST/$APP/Contents/Info" CFBundleShortVersionString 2>/dev/null || echo "?")"
echo "Installed photo-meta $version to $DEST/$APP"
echo "Open it with:  open \"$DEST/$APP\""
