# Building SteBeam from source

SteBeam is a laser beam profiler desktop application for Windows, built around a
Hikrobot MV-CU013-A0UM machine-vision camera. Product overview and feature list: [README.md](README.md).

## Requirements

- OS: Windows 10 x64 or later
- Qt 6.11.x (MinGW build tested with 6.11.2) — the application itself links only
  Qt Widgets (all views are custom-painted); `windeployqt` collects the runtime
  transitive dependencies automatically
- CMake ≥ 3.16 and the MinGW toolchain shipped with Qt
- Hikrobot MVS SDK (for the camera control header at build time; the runtime binds
  `MvCameraControl.dll` dynamically via the MVS runtime, no import library needed)

## Building

```
cmake -S . -B build -G Ninja ^
      -DCMAKE_C_COMPILER=<qt>/Tools/mingw1310_64/bin/gcc.exe ^
      -DCMAKE_CXX_COMPILER=<qt>/Tools/mingw1310_64/bin/g++.exe ^
      -DCMAKE_PREFIX_PATH=<qt>/6.11.2/mingw_64 ^
      -DMVS_SDK_DIR=<MVS SDK>/Development/Includes
cmake --build build
```

`MVS_SDK_DIR` must point to the directory containing `MvCameraControl.h` from the
Hikrobot MVS SDK. The SDK header is not redistributed in this repository.

Deploy the runtime with `windeployqt build/SteBeam.exe`.

## Headless self-tests

```
SteBeam.exe --selftest     # synthetic Gaussian beam vs analytic ground truth
SteBeam.exe --selfcam      # real camera frame-count probe (hardware required)
```

## Trademarks

Hikvision, Hikrobot and MVS
are trademarks of Hangzhou Hikvision Digital Technology Co., Ltd. and its affiliates.
Daheng and the GCI-7103M shutter model designation are trademarks of their
respective owners. Qt is a trademark of The Qt Company. All other trademarks are the
property of their respective owners. Mentions of these marks are for identification
purposes only; this project is not affiliated with or endorsed by any of their owners.

## Third-party components

This software links Qt 6 runtime libraries. The Qt modules shipped with SteBeam
contain third-party components under various open-source licenses; their copyright
notices and license texts are listed in
[LICENSES/THIRD-PARTY-NOTICES.md](LICENSES/THIRD-PARTY-NOTICES.md), with full texts in
`LICENSES/third-party/`.

The GNU LGPLv3 logo used in the About dialog and in the README is an official
public-domain image from the [GNU project](https://www.gnu.org/graphics/license-logos.html).
