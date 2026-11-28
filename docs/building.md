# Building

## Requirements

- Visual Studio 2026 (or 2022) with the C++ workload, and CMake 4.0+.
- [vcpkg](https://github.com/microsoft/vcpkg) cloned somewhere, with `VCPKG_ROOT` pointing at it.
  All C++ dependencies come from vcpkg (`StarCore/vcpkg.json`) on the first configure.
- [uv](https://docs.astral.sh/uv/) for the Python package.

No Vulkan SDK is needed: the shader compiler (glslang) comes from vcpkg.

```powershell
cmake --preset msvc                  # Visual Studio 18 2026 / MSBuild, x64-windows-static-md
cmake --build --preset msvc-release
ctest --preset msvc-release
build\msvc\bin\Release\StarTraveler.exe
```

All executables (the game, the tests, `starcore-render`) end up in one directory per
configuration, `build\msvc\bin\Release\` above. Adding or removing source files re-runs the
configure, which can wait a long time on vcpkg's lock.

## Presets

| Configure preset | Toolchain |
|---|---|
| `msvc` | Visual Studio 18 2026, MSBuild, triplet x64-windows-static-md |
| `msvc-vs2022` | Visual Studio 2022, MSBuild |
| `msvc-ninja` | MSVC with Ninja, inside a VS developer shell |
| `msvc-python` | `msvc` plus the Python extension, built in-tree (see [Python](#python)) |
| `mingw` | GCC from `scoop install mingw-winlibs`. Not checked (see below) |
| `linux-gcc`, `linux-clang` | not tested yet |

The clean build is the `msvc` preset: Visual Studio 2026, MSBuild (the Build Tools are enough;
a full IDE is not required). Where that generator is not installed, `msvc-vs2022` is the same
build with Visual Studio 2022 (`cmake --preset msvc-vs2022`, then
`cmake --build --preset msvc-vs2022-release` and `ctest --preset msvc-vs2022-release`). The
`mingw` preset is not checked.

`VCPKG_ROOT` has to be set to a clone of [vcpkg](https://github.com/microsoft/vcpkg) (`git clone`,
then `bootstrap-vcpkg.bat`). The first configure installs the C++ dependencies and downloads FSR
and DLSS, so CMake has to verify those HTTPS certificates. Kitware's CMake does. The `cmake` that
ships in the WinLibs MinGW package does not use the Windows certificate store: the downloads fail
with `SSL peer certificate or SSH remote key was not OK`. A failed download no longer stops the
configure (the upscaler is left out, as below), but key 6 of the demo then cannot be DLSS-RR. Use
Kitware's CMake, or the one vcpkg fetches on that first run
(`%VCPKG_ROOT%\downloads\tools\cmake-*\bin\cmake.exe`).

Keep the clone on an ASCII path (`C:\src\StarTraveler`, not a folder whose name the system code
page cannot encode). MSBuild compiles shaders by running glslang from a `cmd` batch file, and a
path outside that code page breaks the step: glslang reports `unable to open input file` and
MSBuild reports `The system cannot find the batch label specified - VCEnd`.

Build and test presets append the configuration: `msvc-release`, `msvc-debug`,
`msvc-relwithdebinfo`, `mingw-release`, and so on (`CMakePresets.json` has the full list).

## Options

Set at configure time, e.g. `cmake --preset msvc -DSTARCORE_DLSS=OFF`.

| Option | Default | |
|---|---|---|
| `STARCORE_CXX_STANDARD` | `23` | `26` opts into C++26 (experimental) |
| `STARCORE_IMPORT_STD` | `ON` | the modules `import std;` where the toolchain can (below) |
| `STARCORE_BUILD_GPU` | `ON` | `StarCore::Gpu`: headless Vulkan and the GPU renderer |
| `STARCORE_BUILD_RUNTIME` | `ON` | `StarCore::Runtime`: window, input, ImGui |
| `STARTRAVELER_BUILD_GAME` | `ON` | the game (needs the runtime) |
| `STARCORE_BUILD_TESTS` | `ON` | the C++ tests |
| `STARCORE_BUILD_EXAMPLES` | `ON` | the examples, e.g. `starcore-render` |
| `STARCORE_BUILD_PYTHON` | `OFF` | the Python extension module (`starcore._core`) |
| `STARCORE_VULKAN_VALIDATION` | `OFF` | builds the Vulkan validation layers with vcpkg |
| `STARCORE_FSR` | `ON` | AMD FSR 3.1 (below) |
| `STARCORE_DLSS` | `ON` | NVIDIA DLSS (below; MSVC only) |

## GPU

GPU path tracing needs a GPU with `VK_KHR_ray_query` (NVIDIA RTX, AMD RDNA2+, Intel Arc); without
one path tracing falls back to the CPU, the GPU only rasterizes (any Vulkan 1.3 GPU with push
descriptors) and the ray-tracing GPU tests are skipped.

For development, `-DSTARCORE_VULKAN_VALIDATION=ON` also builds the Vulkan validation layers (with
synchronization validation); the GPU tests and `StarTraveler.exe --validation` then use them.
The environment variable `STARCORE_VULKAN_DEVICE=Intel` (any part of a device name) picks a
specific GPU.

## Upscalers

The vendors' upscalers come from their SDKs, downloaded on configure into
`build/<preset>/_deps/upscalers` (checked against fixed hashes, not part of the repository):

- AMD FSR 3.1: FidelityFX SDK 1.1.4, MIT; its prebuilt Vulkan DLL (Windows, any compiler).
- NVIDIA DLSS: DLSS SDK 310.9.1, NVIDIA's RTX SDK license; its NGX library links with MSVC
  only, so the MinGW build has FSR and the engine's own TAAU.

`-DSTARCORE_FSR=OFF` / `-DSTARCORE_DLSS=OFF` leave them out; without network access they are left
out too.

## C++20 modules

The 2D raster algorithms are the named module `starcore.raster` (`StarCore/modules/`), imported
by the overlay renderer, the editor, the tests and the Python bindings (`import starcore.raster;`);
the rest of the engine is still headers.

With `STARCORE_IMPORT_STD` (default ON) the module's units `import std;` where the toolchain can:
with Visual Studio MSBuild builds the standard library module; with Ninja CMake does, behind an
experimental gate whose key changes with CMake's version (known for CMake 4.2 and 4.4; others fall
back to including the standard headers). Toggling it needs a fresh configure
(`cmake --preset mingw --fresh`): the compiler check is cached.

## Tests

```powershell
ctest --preset msvc-release            # C++ (Catch2): StarCore/tests and StarTraveler/tests
uv run pytest StarCore/python/tests    # Python, after installing the package (below)
```

`StarCore/tests/gpu` checks the GPU renderer against the CPU one; those tests need a GPU with ray
queries, and without one they are skipped.

## Python

The `starcore` package wraps `StarCore::Core` with nanobind and is built by scikit-build-core
through the same CMake project:

```powershell
uv venv
uv pip install -e "./StarCore[test]"
uv run pytest StarCore/python/tests
```

For C++/Python iteration without reinstalling, the `msvc-python` preset builds the extension into
`StarCore/python/starcore` (with the `.venv`'s Python); then `$env:PYTHONPATH = "StarCore/python"`.
An example of the API is in [engine.md](engine.md#python).
