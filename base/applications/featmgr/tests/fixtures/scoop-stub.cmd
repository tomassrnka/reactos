@echo off
rem Stand-in for Scoop in the offline roundtrip test: FEATMGR_SCOOP_RUNNER points here, FEATMGR_SCOOP_ROOT at the fake root.
rem (a failure uses 'exit 7', not 'exit /b 7': nitro's cmd does not return the code of 'exit /b')
rem Records its arguments, and does what Scoop's install/uninstall leave behind: apps\<app>\current.
echo %*>>"%FEATMGR_SCOOP_ROOT%\calls.txt"
if /i "%1"=="install" goto inst
if /i "%1"=="uninstall" goto unin
exit /b 0
:inst
if /i "%2"=="fmfix/fmsc-fail" (
  echo ERROR Hash check failed for 'https://example.invalid/fmsc-fail.zip'. 1>&2
  echo Expected: 4444 Actual: 9999
  exit 7
)
for /f "tokens=1,2 delims=/" %%a in ("%2") do mkdir "%FEATMGR_SCOOP_ROOT%\apps\%%b\current"
exit /b 0
:unin
for /f "tokens=1,2 delims=/" %%a in ("%2") do rd /s /q "%FEATMGR_SCOOP_ROOT%\apps\%%b"
exit /b 0
