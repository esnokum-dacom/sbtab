# SowmCB Tab
This is a program to control the tabulation in [SCBWM](https://github.com/esnokum-dacom/sbcwm) (My window manager modified version of sowm written in XCB)

This probabbly works fine and propperly, as it can work slow and not propperly.
## Dependencies
- libx11
- libgl
- libphread
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


# Limitations
To see the thumbnails, to render it you need to use a compositor (I recommend fastcompmgr because is so optimized and the best option).
