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
