A native macOS photo metadata viewer written in C. Universal build for Apple silicon and Intel Macs, macOS 12 or later.

### Install

**Quickest:** run this in Terminal:

```sh
curl -fsSL https://raw.githubusercontent.com/dmetzger57/photo-meta/main/install.sh | bash
```

**Manual:** download `photo-meta.dmg` below, open it and drag **photo-meta** to Applications.

The app is not notarized by Apple, so the first time you open a manually downloaded copy macOS will say it can't verify the developer. Open **System Settings → Privacy & Security** and click **Open Anyway**, or run:

```sh
xattr -dr com.apple.quarantine /Applications/photo-meta.app
```

The install script doesn't need this step.
