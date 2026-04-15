@echo off
setlocal

REM USD Stage Editor build helper (usd-stage-editor branch)
REM
REM Standard build — uses lib\windows_x64 submodule inside this repo:
REM   make_usd_stage.bat
REM
REM Shared-libs build — point at prebuilt libs from another Blender clone
REM to avoid re-downloading the submodule (useful for multi-worktree setups):
REM   make_usd_stage.bat libdir C:\path\to\other\blender\lib\windows_x64
REM
REM Custom OpenUSD source (skips download + patches in dep-build):
REM   set USD_SOURCE_DIR=C:\path\to\OpenUSD
REM   make_usd_stage.bat
REM
REM All standard make.bat flags work as normal (debug, nobuild, etc.)

set ROOT=%~dp0
set BUILD=%ROOT%..\build_usd_stage

if defined USD_SOURCE_DIR (
    echo Using custom OpenUSD source: %USD_SOURCE_DIR%
    set BUILD_CMAKE_ARGS=-DUSD_SOURCE_DIR="%USD_SOURCE_DIR%"
)

call "%ROOT%make.bat" builddir "%BUILD%" %*

endlocal
