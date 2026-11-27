# Building

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with C++, Clang, CMake and Ninja
- Python 3.10 or newer
- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- A copy of Doritos Crash Course 2

## Assets

TU2 (v0.0.2.5) and Hong Kong DLC are required.

## Steps

Builds with CMake against the ReXGlue SDK.

```powershell
cmake --preset win-amd64-release -DREXSDK_DIR=<path>
cmake --build --preset win-amd64-release
tools/shaderpack/build_shaderpack.ps1
```

