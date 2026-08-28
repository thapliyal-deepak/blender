# Building the USD Stage Editor fork

This is the `usd-stage-editor` branch of a Blender fork that adds a native
**USD Stage Editor** space type. These instructions cover Windows x64.

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| **Git** | any recent | Must support submodules |
| **Visual Studio 2022** | 17.x | Community edition is fine |
| **MSVC toolset** | any 14.4x | Detected automatically by the build system |
| **CMake** | 3.21+ | Bundled with VS or install separately |
| **Ninja** | any | Bundled with VS or `winget install Ninja-build.Ninja` |
| **Python** | 3.11+ | Required by Blender's `make.bat` |

## Clone and submodule setup

```bat
git clone https://projects.blender.org/your-fork/blender-usd-stage.git
cd blender-usd-stage
git checkout usd-stage-editor

REM The prebuilt library submodule is ~20 GB — required for a first build
git submodule update --init lib/windows_x64
```

The `lib/windows_x64` submodule contains all third-party prebuilt libraries
(USD, Python, LLVM, etc.) that Blender's build system expects. You only need
to download it once; subsequent builds reuse it.

## Building

From inside the `blender-usd-stage/` directory:

```bat
make_usd_stage.bat
```

This configures CMake with Ninja and compiles into `..\build_usd_stage\`.
The final executable is:

```
..\build_usd_stage\bin\blender.exe
```

### Build modes

```bat
make_usd_stage.bat              REM Release (default)
make_usd_stage.bat debug        REM Debug build
make_usd_stage.bat nobuild      REM CMake configure only, no compile
```

### Sharing prebuilt libs with another Blender clone

If you already have a full Blender checkout whose `lib/windows_x64` is
populated, you can point this fork at it to avoid a second download:

```bat
make_usd_stage.bat libdir C:\path\to\other\blender\lib\windows_x64
```

### Custom OpenUSD source

To build against your own OpenUSD source tree instead of the bundled version:

```bat
set USD_SOURCE_DIR=C:\path\to\OpenUSD
make_usd_stage.bat
```

## Incremental builds (fast iteration)

After the initial full build, recompile only the USD Stage Editor module:

From the parent `blender-git/` directory:

```bat
build_usd_stage_module.bat
```

This skips the full link and only rebuilds `bf_editor_space_usd_stage`.

> **Note:** `build_usd_stage_module.bat` and `build_ninja.bat` in the parent
> directory pin MSVC to version 14.44.35207. If you have a different toolset
> minor version, use `make_usd_stage.bat` instead — it auto-detects the
> compiler.

## Repository layout

```
blender-usd-stage/
  source/blender/editors/space_usd_stage/   # Space type, operators, panels, tree view
  source/blender/io/usd/intern/usd_stage/   # Runtime, xform/mesh sync, notice handler, matlib
  source/blender/makesdna/DNA_space_types.h # SpaceUsdStage struct
  source/blender/makesrna/intern/rna_space.cc # RNA registration
  lib/windows_x64/                          # Prebuilt third-party libs (submodule)
```

## USD build details

> This section only matters if you are building the prebuilt `lib/windows_x64`
> submodule from source (rare). Normal builds consume the prebuilt libraries
> directly and do not need to read this.

### USD version

This branch builds against **OpenUSD 26.03**
(`build_files/build_environment/cmake/versions.cmake`).
The `lib/windows_x64` submodule was compiled against the same version. If you
point the fork at a `lib/windows_x64` from a different Blender clone, verify
it also ships USD 26.03 or you will get linker errors.

### Patches applied to OpenUSD during a from-source build

Blender's dep-build system applies several patches to OpenUSD before compiling
it. This branch carries the following changes on top of what upstream Blender
applies:

**Removed patches** (fixed in 26.03, no longer needed):
- `usd_mip_trace_3837.diff` — mip-map trace fix (upstreamed)
- `usd_linux_arm64_3764.diff` — Linux arm64 build fix (upstreamed)

**Added patches** (Python shutdown crash fixes):

| Patch | What it fixes |
|-------|---------------|
| `usd_f595276...diff` | `UsdObject.__getattribute__` and `UsdPrimDefinition` — wraps static Python handles in `TfStaticData` so they are intentionally leaked instead of destroyed after `Py_Finalize()` |
| `usd_a609a89...diff` | `tf/pyErrorInternal.cpp` — same fix for the `_ExceptionClass` handle |
| `usd_5744a98...diff` | Boost.Python `function.cpp` — heap-allocates static Python objects so their destructors do not run at shutdown |

All three address the same root cause: Python refcount operations executing in
static destructors after Python has been finalized, causing crashes on exit.

**Updated patches** (line-number drift from 26.03, content unchanged):
- `usd.diff` — removed the `arch/timing.h` rdtsc workaround (no longer needed
  in 26.03); updated `Private.cmake` debug-suffix guard
- `usd_ctor.diff` — Windows PE section names changed from `.pxrctor`/`.pxrdtor`
  to `.pxbctor`/`.pxbdtor` (Blender-specific, avoids collision with Pixar's
  own sections); `extern const` → `static const` to match upstream
- `usd_core_profile.diff` — patch content unchanged, offsets updated for 26.03

## Troubleshooting

**CMake picks the wrong MSVC toolset**
Add `-T version=14.XX.YYYYY` to your cmake invocation, or set
`CMAKE_VS_PLATFORM_TOOLSET_VERSION` in your environment before running
`make_usd_stage.bat`.

**`lib/windows_x64` is empty after clone**
Run `git submodule update --init lib/windows_x64`. The directory exists in
the repo even when the submodule is not initialized.

**Build fails with USD-related linker errors**
Make sure `lib/windows_x64` is at the commit pinned by this branch
(`git submodule status lib/windows_x64`). A mismatch between the submodule
commit and the source branch is the most common cause.
