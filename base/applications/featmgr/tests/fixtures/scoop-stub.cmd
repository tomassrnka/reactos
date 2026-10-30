@echo off
rem Stand-in for Scoop in the offline roundtrip test: FEATMGR_SCOOP_RUNNER points here, FEATMGR_SCOOP_ROOT at the fake root.
rem (a failure uses 'exit 7', not 'exit /b 7': the stub predates the cmd fix and works either way)
rem Records its arguments, and does what Scoop leaves behind:
rem   install <b>/<app>   apps\<app>\current with a copy of the bucket manifest as manifest.json (what "scoop list" reads)
rem   uninstall <b>/<app> removes apps\<app>
rem   update              "git pull" of the buckets: moves pending\*.json into the main bucket
rem   update <app>        installs the bucket's current manifest as the app's current\manifest.json
rem   update *            the same for fmsc-upd (the only app whose manifest changes in the test)
rem   bucket add <n> [u]  creates buckets\<n>\bucket, filled from bucketsrc\*.json
echo %*>>"%FEATMGR_SCOOP_ROOT%\calls.txt"
if /i "%1"=="install" goto inst
if /i "%1"=="uninstall" goto unin
if /i "%1"=="update" goto upd
if /i "%1"=="bucket" goto bkt
exit /b 0
:inst
if /i "%2"=="fmfix/fmsc-fail" (
  echo ERROR Hash check failed for 'https://example.invalid/fmsc-fail.zip'. 1>&2
  echo Expected: 4444 Actual: 9999
  exit 7
)
for /f "tokens=1,2 delims=/" %%a in ("%2") do (
  mkdir "%FEATMGR_SCOOP_ROOT%\apps\%%b\current"
  if exist "%FEATMGR_SCOOP_ROOT%\buckets\%%a\bucket\%%b.json" copy /y "%FEATMGR_SCOOP_ROOT%\buckets\%%a\bucket\%%b.json" "%FEATMGR_SCOOP_ROOT%\apps\%%b\current\manifest.json" >nul
)
exit /b 0
:unin
for /f "tokens=1,2 delims=/" %%a in ("%2") do rd /s /q "%FEATMGR_SCOOP_ROOT%\apps\%%b"
exit /b 0
:upd
if "%2"=="" goto updpull
if "%2"=="*" goto updall
if /i "%2"=="fmsc-failupd" (
  echo ERROR Could not update 'fmsc-failupd': download failed.
  exit 7
)
copy /y "%FEATMGR_SCOOP_ROOT%\buckets\main\bucket\%2.json" "%FEATMGR_SCOOP_ROOT%\apps\%2\current\manifest.json" >nul
exit /b 0
:updpull
if exist "%FEATMGR_SCOOP_ROOT%\pending\fmsc-upd.json" move /y "%FEATMGR_SCOOP_ROOT%\pending\fmsc-upd.json" "%FEATMGR_SCOOP_ROOT%\buckets\main\bucket\fmsc-upd.json" >nul
exit /b 0
:updall
if exist "%FEATMGR_SCOOP_ROOT%\apps\fmsc-upd\current" copy /y "%FEATMGR_SCOOP_ROOT%\buckets\main\bucket\fmsc-upd.json" "%FEATMGR_SCOOP_ROOT%\apps\fmsc-upd\current\manifest.json" >nul
exit /b 0
:bkt
if /i not "%2"=="add" exit /b 0
mkdir "%FEATMGR_SCOOP_ROOT%\buckets\%3\bucket"
if exist "%FEATMGR_SCOOP_ROOT%\bucketsrc" copy /y "%FEATMGR_SCOOP_ROOT%\bucketsrc\*.json" "%FEATMGR_SCOOP_ROOT%\buckets\%3\bucket\" >nul
exit /b 0
