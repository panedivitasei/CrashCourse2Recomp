# Building

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with C++, Clang, CMake and Ninja
- Python 3.10 or newer
- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- Your own copy of Doritos Crash Course 2

## Assets

The base game, TU1 (Media ID: 6FBC14D5) and the Hong Kong DLC are necessary.

## Steps

Builds with CMake against the ReXGlue SDK.

```powershell
cmake --preset win-amd64-release -DREXSDK_DIR=<path>
cmake --build --preset win-amd64-release
tools/shaderpack/build_shaderpack.ps1
```

