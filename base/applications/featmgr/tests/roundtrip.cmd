@echo off
rem FEATMGMT-01 in-guest verb/state round-trip. Run from a directory holding
rem fixtures\ (fmt-base.ini fmt-dep.ini fmt-base.txt fmt-dep.txt).  Result: %O%\fmtest.txt
rem Usage: roundtrip.cmd [fixture-dir]   (default: directory of this script)
setlocal
set FX=%~dp0fixtures
if not "%1"=="" set FX=%1
set FM=%SystemRoot%\system32\featmgr.exe
set R=%SystemRoot%\Featmgr
set O=%~dp0out
mkdir %O% 2>nul
set LOG=%O%\fmtest.txt
set FAILS=0
echo FEATMGMT roundtrip> %LOG%
rem --- install fixtures into the catalog / payload dirs
mkdir %R%\Catalog 2>nul
mkdir %R%\Payload\fmt-base 2>nul
mkdir %R%\Payload\fmt-dep 2>nul
copy /y %FX%\fmt-base.ini %R%\Catalog\ >nul
copy /y %FX%\fmt-dep.ini %R%\Catalog\ >nul
copy /y %FX%\fmt-base.txt %R%\Payload\fmt-base\ >nul
copy /y %FX%\fmt-dep.txt %R%\Payload\fmt-dep\ >nul

call :st fmt-base NotInstalled "1 initial base"
call :st fmt-dep NotInstalled "2 initial dep"

rem install dependent -> pulls in its dependency
%FM% /install fmt-dep >> %LOG% 2>&1
call :rc0 "3 install fmt-dep"
call :st fmt-dep Enabled "4 dep enabled"
call :st fmt-base Enabled "5 base auto-enabled by dependency"
call :ex %R%\fmt-base.out 1 "6 base file placed"
call :ex %R%\fmt-dep.out 1 "7 dep file placed"
reg query HKLM\SOFTWARE\ReactOS\Nitro\FmTest /v Base >nul 2>&1
call :rc0 "8 base reg value set"
reg query HKLM\SOFTWARE\ReactOS\Nitro\FmTest /v Dep >nul 2>&1
call :rc0 "9 dep reg value set"
%FM% /query fmt-base > %O%\q.txt 2>&1
type %O%\q.txt >> %LOG%
find "EnabledDependents: fmt-dep" %O%\q.txt >nul
call :rc0 "10 query reports enabled dependent"
%FM% /list /state:enabled > %O%\l.txt 2>&1
find "fmt-dep" %O%\l.txt >nul
call :rc0 "11 list /state:enabled shows dep"
%FM% /list /tier:software > %O%\l.txt 2>&1
find "fmt-base" %O%\l.txt >nul
call :rc1 "12 list /tier:software hides os-tier base"

rem dependency protection
%FM% /disable fmt-base >> %LOG% 2>&1
call :rc1 "13 disable base refused while dep enabled"
call :st fmt-base Enabled "14 base still enabled"
call :ex %R%\fmt-base.out 1 "15 base file still present"

rem proper order
%FM% /disable fmt-dep >> %LOG% 2>&1
call :rc0 "16 disable dep"
call :st fmt-dep Disabled "17 dep Disabled"
call :ex %R%\fmt-dep.out 0 "18 dep file removed"
reg query HKLM\SOFTWARE\ReactOS\Nitro\FmTest /v Dep >nul 2>&1
call :rc1 "19 dep reg value reverted"
call :st fmt-base Enabled "20 base untouched"
%FM% /disable fmt-base >> %LOG% 2>&1
call :rc0 "21 disable base"
call :st fmt-base Disabled "22 base Disabled"
call :ex %R%\fmt-base.out 0 "23 base file removed"
reg query HKLM\SOFTWARE\ReactOS\Nitro\FmTest /v Base >nul 2>&1
call :rc1 "24 base reg value reverted"

rem re-enable + cascade disable
%FM% /enable fmt-dep >> %LOG% 2>&1
call :rc0 "25 re-enable dep (re-enables base)"
call :st fmt-base Enabled "26 base Enabled again"
call :st fmt-dep Enabled "27 dep Enabled again"
%FM% /disable fmt-base /cascade >> %LOG% 2>&1
call :rc0 "28 cascade disable base"
call :st fmt-dep Disabled "29 dep Disabled by cascade"
call :st fmt-base Disabled "30 base Disabled by cascade"

rem uninstall -> forget
%FM% /uninstall fmt-dep >> %LOG% 2>&1
call :rc0 "31 uninstall dep"
%FM% /uninstall fmt-base >> %LOG% 2>&1
call :rc0 "32 uninstall base"
call :st fmt-dep NotInstalled "33 dep NotInstalled after uninstall"
call :st fmt-base NotInstalled "34 base NotInstalled after uninstall"
reg query HKLM\SOFTWARE\ReactOS\Nitro\OptionalFeatures\fmt-base >nul 2>&1
call :rc1 "35 base state record deleted"
rem uninstall of an enabled feature = disable + forget in one step
%FM% /install fmt-base >> %LOG% 2>&1
%FM% /uninstall fmt-base >> %LOG% 2>&1
call :rc0 "36 uninstall of enabled feature"
call :st fmt-base NotInstalled "37 NotInstalled after enabled-uninstall"
call :ex %R%\fmt-base.out 0 "38 base file gone"

rem --- Removable=0 (base component) guards (GFX-FU), coexisting with the dependency guard
rem the real shipped lavapipe-pack manifest (Removable=0), if staged next to the fixtures
if exist %FX%\lavapipe-pack.ini copy /y %FX%\lavapipe-pack.ini %R%\Catalog\ >nul
if exist %FX%\lavapipe-pack.ini (
  %FM% /disable lavapipe-pack > %O%\b.txt 2>&1
  call :rc1 "39 disable lavapipe-pack refused"
  find "base component" %O%\b.txt >nul
  call :rc0 "40 ...with the base-component error"
  %FM% /uninstall lavapipe-pack > %O%\b.txt 2>&1
  call :rc1 "41 uninstall lavapipe-pack refused"
  find "base component" %O%\b.txt >nul
  call :rc0 "42 ...with the base-component error"
)
mkdir %R%\Payload\fmt-sys 2>nul
mkdir %R%\Payload\fmt-sysdep 2>nul
copy /y %FX%\fmt-sys.ini %R%\Catalog\ >nul
copy /y %FX%\fmt-sysdep.ini %R%\Catalog\ >nul
copy /y %FX%\fmt-sys.txt %R%\Payload\fmt-sys\ >nul
copy /y %FX%\fmt-sysdep.txt %R%\Payload\fmt-sysdep\ >nul
%FM% /install fmt-sysdep >> %LOG% 2>&1
call :rc0 "43 install dependent of base component"
call :st fmt-sys Enabled "44 base component enabled"
rem Removable guard wins over the dependency guard
%FM% /disable fmt-sys > %O%\b.txt 2>&1
call :rc1 "45 disable base component refused (dependent present)"
find "base component" %O%\b.txt >nul
call :rc0 "46 ...base-component error takes precedence over dependents"
%FM% /disable fmt-sysdep >> %LOG% 2>&1
call :rc0 "47 disable dependent"
%FM% /disable fmt-sys > %O%\b.txt 2>&1
call :rc1 "48 disable base component refused (no dependents)"
find "base component" %O%\b.txt >nul
call :rc0 "49 ...base-component error"
%FM% /uninstall fmt-sys > %O%\b.txt 2>&1
call :rc1 "50 uninstall base component refused"
call :st fmt-sys Enabled "51 base component still Enabled"
call :ex %R%\fmt-sys.out 1 "52 base component file still present"
rem the fixture is force-removed by hand (the engine rightly will not)
%FM% /uninstall fmt-sysdep >> %LOG% 2>&1
reg delete HKLM\SOFTWARE\ReactOS\Nitro\OptionalFeatures\fmt-sys /f >nul 2>&1
del /q %R%\fmt-sys.out 2>nul
del /q %R%\Catalog\fmt-sys.ini %R%\Catalog\fmt-sysdep.ini 2>nul
del /q %R%\Payload\fmt-sys\fmt-sys.txt %R%\Payload\fmt-sysdep\fmt-sysdep.txt 2>nul
rmdir %R%\Payload\fmt-sys 2>nul
rmdir %R%\Payload\fmt-sysdep 2>nul

rem ===== SOFTWARE TIER (FEATMGMT-01 software slice) =====
rem --- A. /import-rapps: RAPPS appdb -> Tier=software catalog entries
set SW=%R%\Catalog\Software
rd /s /q %SW% 2>nul
%FM% /import-rapps %FX%\sw-appdb /anyarch > %O%\imp.txt 2>&1
call :rc0 "53 import-rapps rc0"
type %O%\imp.txt >> %LOG%
find "imported 5 RAPPS entries" %O%\imp.txt >nul
call :rc0 "54 imported 5 (testapp testlib depapp archapp utf16app)"
find "(2 skipped)" %O%\imp.txt >nul
call :rc0 "55 2 skipped (no URL, unusable package name)"
%FM% /list /tier:software > %O%\l.txt 2>&1
find "rapps-testapp" %O%\l.txt >nul
call :rc0 "56 imported entry listed in software tier"
%FM% /list /tier:os > %O%\l.txt 2>&1
find "rapps-testapp" %O%\l.txt >nul
call :rc1 "57 imported entry absent from os tier"
call :st rapps-testapp NotInstalled "58 imported entry NotInstalled"
%FM% /query rapps-testapp > %O%\q.txt 2>&1
type %O%\q.txt >> %LOG%
find "Tier:         software" %O%\q.txt >nul
call :rc0 "59 query: Tier=software"
find "Provider:     rapps" %O%\q.txt >nul
call :rc0 "60 query: Provider=rapps"
find "Rapps.Pkg:    testapp" %O%\q.txt >nul
call :rc0 "61 query: Rapps.Pkg"
find "Version:      1.2.3" %O%\q.txt >nul
call :rc0 "62 query: Version mapped"
find "Source.Artifact: https://example.invalid/testapp-1.2.3.exe" %O%\q.txt >nul
call :rc0 "63 query: URLDownload -> Source.Artifact"
find "Source.ArtifactSHA1: 0123456789abcdef0123456789abcdef01234567" %O%\q.txt >nul
call :rc0 "64 query: SHA1 -> Source.ArtifactSHA1"
find "Source.License: Open Source (GPLv2)" %O%\q.txt >nul
call :rc0 "65 query: License/LicenseType -> Source.License"
find "Category:     Internet" %O%\q.txt >nul
call :rc0 "66 query: numeric Category -> name"
%FM% /query rapps-depapp > %O%\q.txt 2>&1
find "Dependencies: rapps-testlib" %O%\q.txt >nul
call :rc0 "67 RAPPS Dependencies -> catalog deps (present pkgs only)"
find "nosuchpkg" %O%\q.txt >nul
call :rc1 "68 dep absent from the DB not in the catalog graph"
%FM% /query rapps-archapp > %O%\q.txt 2>&1
find "Source.Artifact: https://example.invalid/arch-" %O%\q.txt >nul
call :rc0 "69 [Section.<arch>] URLDownload overrides generic"
%FM% /query rapps-utf16app > %O%\q.txt 2>&1
call :rc0 "70 UTF-16LE+BOM RAPPS entry imported"
%FM% /query rapps-nourl >nul 2>&1
call :rc1 "71 entry without URLDownload not imported"
rem re-run is additive/idempotent; /clean drops stale generated entries only
echo [Feature]> %SW%\rapps-stale.ini
echo Id=rapps-stale>> %SW%\rapps-stale.ini
echo Tier=software>> %SW%\rapps-stale.ini
%FM% /query rapps-stale >nul 2>&1
call :rc0 "72 hand-made stale entry visible"
%FM% /import-rapps %FX%\sw-appdb /anyarch > %O%\imp.txt 2>&1
find "imported 5 RAPPS entries" %O%\imp.txt >nul
call :rc0 "73 re-import idempotent"
%FM% /query rapps-stale >nul 2>&1
call :rc0 "74 re-import without /clean keeps other entries"
%FM% /import-rapps %FX%\sw-appdb /anyarch /clean > %O%\imp.txt 2>&1
%FM% /query rapps-stale >nul 2>&1
call :rc1 "75 /clean removes stale generated entries"
%FM% /import-rapps %R%\nonexistent-dir > %O%\imp.txt 2>&1
call :rc1 "76 import from a missing dir fails (rc 3)"

rem --- B. software-tier install semantics (command provider = controlled local installer)
mkdir %R%\Payload\fmt-sw 2>nul
mkdir %R%\Payload\fmt-swbad 2>nul
mkdir %R%\Payload\fmt-swstuck 2>nul
copy /y %FX%\fmt-sw.ini %R%\Catalog\ >nul
copy /y %FX%\fmt-swbad.ini %R%\Catalog\ >nul
copy /y %FX%\fmt-swstuck.ini %R%\Catalog\ >nul
copy /y %FX%\fmt-sw-setup.cmd %R%\Payload\fmt-sw\ >nul
copy /y %FX%\fmt-sw-uninst.cmd %R%\Payload\fmt-sw\ >nul
copy /y %FX%\fmt-swbad-setup.cmd %R%\Payload\fmt-swbad\ >nul
copy /y %FX%\fmt-swstuck-setup.cmd %R%\Payload\fmt-swstuck\ >nul
copy /y %FX%\fmt-swstuck-uninst.cmd %R%\Payload\fmt-swstuck\ >nul
call :st fmt-sw NotInstalled "77 sw initial"
%FM% /install fmt-sw > %O%\i.txt 2>&1
call :rc0 "78 install runs the installer (rc0)"
type %O%\i.txt >> %LOG%
call :st fmt-sw Enabled "79 sw Enabled after install"
call :ex %R%\fmt-sw.installed 1 "80 installer ran (side-effect file)"
reg query HKLM\SOFTWARE\ReactOS\Nitro\FmTest /v Sw >nul 2>&1
call :rc0 "81 installer side-effect registry value"
%FM% /list /state:enabled /tier:software > %O%\l.txt 2>&1
find "fmt-sw" %O%\l.txt >nul
call :rc0 "82 list shows installed software"
%FM% /disable fmt-sw > %O%\i.txt 2>&1
call :rc0 "83 disable runs the uninstaller (rc0)"
type %O%\i.txt >> %LOG%
call :st fmt-sw Disabled "84 sw Disabled"
call :ex %R%\fmt-sw.installed 0 "85 uninstaller removed side-effect file"
reg query HKLM\SOFTWARE\ReactOS\Nitro\FmTest /v Sw >nul 2>&1
call :rc1 "86 uninstaller removed registry value"
%FM% /enable fmt-sw >> %LOG% 2>&1
call :rc0 "87 re-install from Disabled"
call :st fmt-sw Enabled "88 sw Enabled again"
%FM% /uninstall fmt-sw >> %LOG% 2>&1
call :rc0 "89 uninstall (uninstaller + forget)"
call :st fmt-sw NotInstalled "90 sw NotInstalled after uninstall"
reg query HKLM\SOFTWARE\ReactOS\Nitro\OptionalFeatures\fmt-sw >nul 2>&1
call :rc1 "91 sw state record deleted"
call :ex %R%\fmt-sw.installed 0 "92 sw side-effect file gone"
rem failing installer: feature stays NotInstalled, no record left behind
%FM% /install fmt-swbad > %O%\i.txt 2>&1
call :rc1 "93 failing installer -> install fails (rc3)"
type %O%\i.txt >> %LOG%
call :st fmt-swbad NotInstalled "94 failed install leaves NotInstalled"
reg query HKLM\SOFTWARE\ReactOS\Nitro\OptionalFeatures\fmt-swbad >nul 2>&1
call :rc1 "95 failed install leaves no state record"
rem failing uninstaller: stays Enabled unless /force
%FM% /install fmt-swstuck >> %LOG% 2>&1
call :rc0 "96 swstuck installs"
%FM% /disable fmt-swstuck > %O%\i.txt 2>&1
call :rc1 "97 failing uninstaller -> disable refused"
type %O%\i.txt >> %LOG%
call :st fmt-swstuck Enabled "98 swstuck still Enabled"
%FM% /uninstall fmt-swstuck >> %LOG% 2>&1
call :rc1 "99 uninstall refused too"
%FM% /uninstall fmt-swstuck /force >> %LOG% 2>&1
call :rc0 "100 uninstall /force forgets it"
call :st fmt-swstuck NotInstalled "101 swstuck NotInstalled after /force"
rem installer not staged -> deferred, refused
del /q %R%\Payload\fmt-sw\fmt-sw-setup.cmd
%FM% /list /tier:software > %O%\l.txt 2>&1
find "fmt-sw " %O%\l.txt | find "*" >nul
call :rc0 "102 software entry with missing installer marked deferred (*)"
%FM% /install fmt-sw >> %LOG% 2>&1
call :rc1 "103 install refused when installer not staged"

rem --- software-tier cleanup
del /q %R%\Catalog\fmt-sw.ini %R%\Catalog\fmt-swbad.ini %R%\Catalog\fmt-swstuck.ini 2>nul
del /q %R%\Payload\fmt-sw\*.* %R%\Payload\fmt-swbad\*.* %R%\Payload\fmt-swstuck\*.* 2>nul
rmdir %R%\Payload\fmt-sw 2>nul
rmdir %R%\Payload\fmt-swbad 2>nul
rmdir %R%\Payload\fmt-swstuck 2>nul
rd /s /q %SW% 2>nul

rem --- cleanup fixtures
del /q %R%\Catalog\fmt-base.ini %R%\Catalog\fmt-dep.ini 2>nul
del /q %R%\Payload\fmt-base\fmt-base.txt %R%\Payload\fmt-dep\fmt-dep.txt 2>nul
rmdir %R%\Payload\fmt-base 2>nul
rmdir %R%\Payload\fmt-dep 2>nul
reg delete HKLM\SOFTWARE\ReactOS\Nitro\FmTest /f >nul 2>&1

if "%FAILS%"=="0" (echo ROUNDTRIP-RESULT: PASS>> %LOG%) else (echo ROUNDTRIP-RESULT: FAIL %FAILS%>> %LOG%)
goto :eof

:st
%FM% /state %1 > %O%\s.txt 2>&1
find "%2" %O%\s.txt >nul
if errorlevel 1 (call :fail %3 "state != %2") else (echo PASS %3>> %LOG%)
goto :eof
:ex
if exist %1 (set GOT=1) else (set GOT=0)
if "%GOT%"=="%2" (echo PASS %3>> %LOG%) else (call :fail %3 "exist=%GOT% want %2")
goto :eof
:rc0
if errorlevel 1 (call :fail %1 "rc nonzero") else (echo PASS %1>> %LOG%)
goto :eof
:rc1
if errorlevel 1 (echo PASS %1>> %LOG%) else (call :fail %1 "rc zero, wanted failure")
goto :eof
:fail
echo FAIL %~1 -- %~2>> %LOG%
set /a FAILS=%FAILS%+1
goto :eof
