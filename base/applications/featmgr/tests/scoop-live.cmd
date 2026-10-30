@echo off
rem SCOOP-FEATMGR-01 live proof (amd64, NTFS, needs internet). Run inside the guest:
rem   scoop-live.cmd [extras]
rem Steps: featmgr /install scoop (pulls in powershell-7 when missing) -> /import-scoop main ->
rem /install 7zip -> 7z runs -> /uninstall scoop refused while 7zip is installed -> /uninstall 7zip ->
rem /uninstall scoop. "extras" adds: /install git, scoop bucket add extras, /import-scoop extras.
rem Result: %O%\scooplive.txt, last line SCOOPLIVE-RESULT: PASS|FAIL n
setlocal
set FM=%SystemRoot%\system32\featmgr.exe
set O=C:\harness\out
set SROOT=C:\scoop
set PSDIR=C:\Program Files\PowerShell\7
set ENVKEY=HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment
set LOG=%O%\scooplive.txt
set FAILS=0
mkdir %O% 2>nul
echo Scoop live test %date% %time%> %LOG%

rem the guest needs its DHCP lease before the first download
set N=0
:netwait
ping -n 1 -w 1000 github.com 2>&1 | find "Reply from" >nul
if not errorlevel 1 goto netup
set /a N+=1
if %N% GEQ 120 goto netup
ping -n 3 127.0.0.1 >nul
goto netwait
:netup
echo network wait: %N% tries>> %LOG%

%FM% /list /tier:software > %O%\l.txt 2>&1
find "scoop" %O%\l.txt >nul
call :rc0 "L1 scoop listed by featmgr /list"
call :st scoop NotInstalled "L2 scoop NotInstalled at start"
if exist "%SROOT%" (call :fail "L3 no C:\scoop at start") else (call :pass "L3 no C:\scoop at start")
call :stq powershell-7

rem --- install scoop (installs powershell-7 first when it is missing)
%FM% /install scoop > %O%\install-scoop.txt 2>&1
call :rc0 "L4 featmgr /install scoop rc0"
type %O%\install-scoop.txt >> %LOG%
call :st scoop Enabled "L5 scoop Enabled"
call :st powershell-7 Enabled "L6 powershell-7 Enabled (dependency)"
if exist "%PSDIR%\pwsh.exe" (call :pass "L7 pwsh.exe installed") else (call :fail "L7 pwsh.exe installed")
if exist "%SROOT%\shims\scoop.cmd" (call :pass "L8 scoop shim present") else (call :fail "L8 scoop shim present")
if exist "%SROOT%\apps\scoop\current\bin\scoop.ps1" (call :pass "L9 scoop core present") else (call :fail "L9 scoop core present")
if exist "%SROOT%\buckets\main\bucket" (call :pass "L10 main bucket present") else (call :fail "L10 main bucket present")
reg query "%ENVKEY%" /v SCOOP | find "C:\scoop" >nul
call :rc0 "L11 machine SCOOP variable set"
reg query "%ENVKEY%" /v Path | find "C:\scoop\shims" >nul
call :rc0 "L12 machine PATH has C:\scoop\shims"
set SCOOP=%SROOT%
set PATH=%SROOT%\shims;%PSDIR%;%PATH%
call scoop --version > %O%\ver.txt 2>&1
type %O%\ver.txt >> %LOG%
find "Current Scoop version" %O%\ver.txt >nul
call :rc0 "L13 scoop --version runs from cmd"

rem --- import the main bucket
%FM% /import-scoop main > %O%\import.txt 2>&1
call :rc0 "L14 featmgr /import-scoop main rc0"
find "imported" %O%\import.txt | find "Scoop app(s)" >nul
call :rc0 "L15 import summary printed"
type %O%\import.txt | find "imported" >> %LOG%
%FM% /query 7zip > %O%\q.txt 2>&1
type %O%\q.txt >> %LOG%
find "Source:       scoop/main" %O%\q.txt >nul
call :rc0 "L16 7zip entry from scoop/main"
find "Scoop.Verified: 1" %O%\q.txt >nul
call :rc0 "L17 7zip manifest carries a hash"
%FM% /list /tier:software /source:scoop/main > %O%\l.txt 2>&1
find "git " %O%\l.txt >nul
call :rc0 "L18 git is listed too"
call :st 7zip NotInstalled "L19 7zip NotInstalled"

rem --- install 7zip through featmgr, then run it
%FM% /install 7zip > %O%\install-7zip.txt 2>&1
call :rc0 "L20 featmgr /install 7zip rc0"
type %O%\install-7zip.txt >> %LOG%
call :st 7zip Enabled "L21 7zip Enabled"
if exist "%SROOT%\apps\7zip\current\7z.exe" (call :pass "L22 7z.exe in apps\7zip\current") else (call :fail "L22 7z.exe in apps\7zip\current")
if exist "%SROOT%\shims\7z.exe" (call :pass "L23 7z shim created") else (call :fail "L23 7z shim created")
"%SROOT%\shims\7z.exe" > %O%\7z.txt 2>&1
type %O%\7z.txt | find "7-Zip" >> %LOG%
find "7-Zip" %O%\7z.txt >nul
call :rc0 "L24 7z runs (banner)"
mkdir %O%\z7 2>nul
echo scoop-featmgr-7z-payload> %O%\z7\in.txt
"%SROOT%\shims\7z.exe" a -y %O%\z7\t.7z %O%\z7\in.txt > %O%\7za.txt 2>&1
call :rc0 "L25 7z a (create archive) rc0"
"%SROOT%\shims\7z.exe" e -y -o%O%\z7\out %O%\z7\t.7z > %O%\7ze.txt 2>&1
call :rc0 "L26 7z e (extract) rc0"
find "scoop-featmgr-7z-payload" %O%\z7\out\in.txt >nul
call :rc0 "L27 extracted content matches"
rd /s /q %O%\z7 2>nul

rem --- optional (SHOT=1, VM harness): the Optional Features GUI on the Software tab, for screenshots
if not defined SHOT goto :noshot
start "" %SystemRoot%\system32\optfeatures.exe /software /source:scoop/main
ping -n 15 127.0.0.1 >nul
echo SCOOPLIVE:SHOT1>COM2
ping -n 20 127.0.0.1 >nul
taskkill /f /im optfeatures.exe >nul 2>&1
start "" %SystemRoot%\system32\optfeatures.exe /software
ping -n 15 127.0.0.1 >nul
echo SCOOPLIVE:SHOT2>COM2
ping -n 20 127.0.0.1 >nul
taskkill /f /im optfeatures.exe >nul 2>&1
:noshot

rem --- dependency protection, then uninstall in order
%FM% /uninstall scoop > %O%\un-early.txt 2>&1
call :rc1 "L28 /uninstall scoop refused while 7zip is installed"
find "7zip" %O%\un-early.txt >nul
call :rc0 "L29 ...naming 7zip"
if exist "%SROOT%\shims\scoop.cmd" (call :pass "L30 scoop still there") else (call :fail "L30 scoop still there")

if /i not "%1"=="extras" goto :uninst
rem --- optional: extras bucket (needs git, itself a Scoop app)
%FM% /import-scoop extras > %O%\imp-x0.txt 2>&1
call :rc1 "E1 /import-scoop extras refused before the bucket is added"
find "scoop bucket add extras" %O%\imp-x0.txt >nul
call :rc0 "E2 ...with the exact command to run"
%FM% /install git > %O%\install-git.txt 2>&1
call :rc0 "E3 featmgr /install git rc0"
type %O%\install-git.txt >> %LOG%
call scoop bucket add extras > %O%\bucket.txt 2>&1
type %O%\bucket.txt >> %LOG%
%FM% /scoop-buckets > %O%\b.txt 2>&1
type %O%\b.txt >> %LOG%
find "extras" %O%\b.txt >nul
call :rc0 "E4 scoop bucket add extras done (listed)"
%FM% /import-scoop extras > %O%\imp-x.txt 2>&1
call :rc0 "E5 featmgr /import-scoop extras rc0"
type %O%\imp-x.txt | find "imported" >> %LOG%
%FM% /list /tier:software /source:scoop/extras > %O%\l.txt 2>&1
find "feature(s)" %O%\l.txt >> %LOG%
%FM% /uninstall git >> %LOG% 2>&1
call :rc0 "E6 featmgr /uninstall git rc0"

:uninst
%FM% /uninstall 7zip > %O%\un-7zip.txt 2>&1
call :rc0 "U1 featmgr /uninstall 7zip rc0"
type %O%\un-7zip.txt >> %LOG%
call :st 7zip NotInstalled "U2 7zip NotInstalled"
if exist "%SROOT%\apps\7zip" (call :fail "U3 apps\7zip removed") else (call :pass "U3 apps\7zip removed")
if exist "%SROOT%\shims\7z.exe" (call :fail "U4 7z shim removed") else (call :pass "U4 7z shim removed")

%FM% /uninstall scoop > %O%\un-scoop.txt 2>&1
call :rc0 "U5 featmgr /uninstall scoop rc0"
type %O%\un-scoop.txt >> %LOG%
call :st scoop NotInstalled "U6 scoop NotInstalled"
if exist "%SROOT%" (call :fail "U7 C:\scoop removed") else (call :pass "U7 C:\scoop removed")
reg query "%ENVKEY%" /v SCOOP >nul 2>&1
call :rc1 "U8 machine SCOOP variable removed"
reg query "%ENVKEY%" /v Path | find "C:\scoop" >nul
call :rc1 "U9 machine PATH no longer has C:\scoop"
call :st powershell-7 Enabled "U10 powershell-7 left in place (not Scoop's to remove)"

if "%FAILS%"=="0" (echo SCOOPLIVE-RESULT: PASS>> %LOG%) else (echo SCOOPLIVE-RESULT: FAIL %FAILS%>> %LOG%)
goto :eof

:stq
%FM% /state %1 > %O%\s.txt 2>&1
echo %1 state at start: >> %LOG%
type %O%\s.txt >> %LOG%
goto :eof

:st
%FM% /state %1 > %O%\s.txt 2>&1
find "%2" %O%\s.txt >nul
if errorlevel 1 (call :fail %3) else (call :pass %3)
goto :eof

:rc0
if errorlevel 1 (call :fail %1) else (call :pass %1)
goto :eof

:rc1
if errorlevel 1 (call :pass %1) else (call :fail %1)
goto :eof

:pass
echo PASS %~1>> %LOG%
goto :eof

:fail
echo FAIL %~1>> %LOG%
set /a FAILS+=1
goto :eof
