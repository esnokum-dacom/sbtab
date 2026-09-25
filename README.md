# SowmCB Tab
This is a program to control the tabulation in [SCBWM](https://github.com/esnokum-dacom/sbcwm) (My window manager modified version of sowm written in XCB)

This probabbly works fine and propperly, as it can work slow and not propperly.
## Dependencies
- libx11
- libgl
- libpng
- libpthread
- Latest c compiler
- CMake

# Build
Simply clone and run. Ensure CMake is installed
```
cmake --build build
```
in the project directory. // home/user/etc/sbtb.


# install
I have no planned to up in any package manager, you can install copying the repo and using
```
(if is neccesary sudo) cmake --install build
```

## Usage
First start the daemon by running:
```
sbtb
```

# FEATURES
It can automatically focus the window (client) is selected.
You can move with hjkl, arrows and use alt-tab to move.
- Transparent background when a compositor (picom, KWin, etc.) is running; otherwise it falls back to a black background that covers the monitor.
- While a compositor is running it also requests a background **blur** on just the drawn cells (thumbnail + label), so it blends with the desktop without blurring the whole screen.
- Client thumbnails are captured via the XComposite extension (`CompositeNameWindowPixmap`) only while a compositor is running, so they render with any compositor.
- Without a compositor, each client is shown as a icon: the client's own icon (`_NET_WM_ICON`, as set by kitty, firefox, etc.) when available, otherwise the bundled `dicon.png` default icon. The icon is drawn centered in the cell.
- The default icon is looked up from `$SBTB_ICON`, the directory of the executable, or the current/source directory.
- The window sets `WM_CLASS` to `sbtb` so compositors can match it in their rules.

# Blur
The blur effect is requested through the `_KDE_NET_WM_BLUR_BEHIND_REGION` / `_NET_WM_BLUR_BEHIND_REGION` hints,
which KWin (and other KDE-style compositors) honor directly.

For **picom**, per-window blur is configured with rules. To keep it cheap, use the `dual_kawase` method with a
small strength. Example `picom.conf` snippet:

```
blur-background = true;
blur-method = "dual_kawase";
blur-strength = 5;
blur-background-exclude = [ "class_g = 'sbtb' && window_type = 'normal'" ];
blur-background-frame = false;

wintypes: {
  normal = { blur-background = true; };
};
```

Or match per window class:

```
rules: {
  { match = "class_g = 'sbtb'"; blur-background = true; blur-method = "dual_kawase"; blur-strength = 5; };
};
```

You can force blur off for the switcher (opaque transparent, no blur) by deleting the hints:
`xprop -id <id> -remove _KDE_NET_WM_BLUR_BEHIND_REGION -remove _NET_WM_BLUR_BEHIND_REGION`.

# Limitations
No compositor is needed for the tab bar itself; it renders as a normal opaque window filling the monitor when no compositor is detected. Blur depends on the compositor supporting the blur-behind hint (KWin does; picom needs the per-window rule above).
