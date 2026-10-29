@echo off
rem FEATMGMT software tier: PowerShell 7 live proof (amd64 only; needs internet).
rem   powershell7-live.cmd install     negative checksum test, then install powershell-7 and check it
rem   powershell7-live.cmd uninstall   uninstall powershell-7 and check everything is gone
rem Run from a directory holding fixtures\fmt-ps7bad.ini and fixtures\fmt-ps7bad.txt (default: this script's
rem directory). Result: %O%\ps7live.txt, last line PS7LIVE-RESULT: PASS|FAIL n
setlocal
set FX=%~dp0fixtures
set FM=%SystemRoot%\system32\featmgr.exe
set R=%SystemRoot%\Featmgr
set O=C:\harness\out
set PSDIR=C:\Program Files\PowerShell\7
set ENVKEY=HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment
set LOG=%O%\ps7live.txt
set FAILS=0
mkdir %O% 2>nul
echo PowerShell 7 live test %1 %date% %time%>> %LOG%

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

if /i "%1"=="uninstall" goto :uninst

call :present "I1 powershell-7 listed by /list"
%FM% /query powershell-7 > %O%\q.txt 2>&1
find "ArtifactSHA256: " %O%\q.txt >nul
call :rc0 "I2 manifest pins a SHA-256"
if exist C:\Windows\System32\pwrshplugin.dll (call :fail "I3 no pwrshplugin.dll faked (found one)") else (call :pass "I3 no pwrshplugin.dll faked")

rem --- wrong SHA-256 must be refused after the download: nothing installed, no record
copy /y %FX%\fmt-ps7bad.ini %R%\Catalog\ >nul
copy /y %FX%\fmt-ps7bad.txt %R%\ >nul
%FM% /install fmt-ps7bad > %O%\bad.txt 2>&1
call :rc1 "I4 install with a wrong SHA-256 fails"
type %O%\bad.txt >> %LOG%
if exist "%PSDIR%\pwsh.exe" (call :fail "I5 nothing installed after the checksum failure") else (call :pass "I5 nothing installed after the checksum failure")
call :st fmt-ps7bad NotInstalled "I6 wrong-hash entry left NotInstalled"
del /q %R%\Catalog\fmt-ps7bad.ini %R%\fmt-ps7bad.txt 2>nul

rem --- the real install
%FM% /install powershell-7 > %O%\install.txt 2>&1
call :rc0 "I7 featmgr /install powershell-7 rc0"
type %O%\install.txt >> %LOG%
call :st powershell-7 Enabled "I8 Enabled after install"
if exist "%PSDIR%\pwsh.exe" (call :pass "I9 pwsh.exe installed") else (call :fail "I9 pwsh.exe installed")
reg query "%ENVKEY%" /v Path | find "PowerShell\7" >nul
call :rc0 "I10 system PATH has the PowerShell 7 folder"
if exist "%ALLUSERSPROFILE%\Start Menu\Programs\PowerShell\PowerShell 7 (x64).lnk" (call :pass "I11 Start-menu shortcut present") else (call :fail "I11 Start-menu shortcut present")
"%PSDIR%\pwsh.exe" -NoProfile -Command "$PSVersionTable.PSVersion.ToString()" > %O%\ver.txt 2>&1
type %O%\ver.txt >> %LOG%
find "7.4." %O%\ver.txt >nul
call :rc0 "I12 pwsh runs and reports 7.4.x"
reg query "HKLM\SOFTWARE\Microsoft\PowerShell\3" >nul 2>&1
call :rc1 "I13 no PowerShell\3 key faked"
goto :end

:uninst
%FM% /uninstall powershell-7 > %O%\un.txt 2>&1
call :rc0 "U1 featmgr /uninstall powershell-7 rc0"
type %O%\un.txt >> %LOG%
call :st powershell-7 NotInstalled "U2 NotInstalled after uninstall"
if exist "%PSDIR%\pwsh.exe" (call :fail "U3 pwsh.exe removed") else (call :pass "U3 pwsh.exe removed")
reg query "%ENVKEY%" /v Path | find "PowerShell\7" >nul
call :rc1 "U4 PowerShell 7 folder gone from the system PATH"
if exist "%ALLUSERSPROFILE%\Start Menu\Programs\PowerShell\PowerShell 7 (x64).lnk" (call :fail "U5 Start-menu shortcut removed") else (call :pass "U5 Start-menu shortcut removed")
goto :end

:end
if "%FAILS%"=="0" (echo PS7LIVE-RESULT: PASS>> %LOG%) else (echo PS7LIVE-RESULT: FAIL %FAILS%>> %LOG%)
goto :eof

:present
%FM% /list > %O%\l.txt 2>&1
find "powershell-7" %O%\l.txt >nul
if errorlevel 1 (call :fail %1) else (call :pass %1)
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
