@echo off
rem Uninstall of the Microsoft .NET Framework 4.8 on amd64 (featmgr: UninstallCmd.amd64 of dotnet-framework-4.8.ini).
rem
rem The Framework's own Setup.exe /uninstall is what runs on i386. On amd64 it ends with success and removes nothing:
rem it decides that nothing is installed because MsiGetPatchInfoEx cannot find the .NET 4.8 update (KB4503575), whose
rem registration the 32-bit Setup.exe's MsiInstallProduct could not complete (a relative RegCreateKeyEx from a handle that
rem was opened with KEY_WOW64_64KEY lands in the 32-bit view, so the product's Patches key stays empty).
rem Until that is fixed this runs the uninstaller the product registers itself ("MsiExec.exe /X{product code}", the
rem x64 netfx_Full_x64.msi, which also carries the 32-bit Framework) and then removes what only Setup.exe would have
rem removed: its Add/Remove Programs entries, the update record and its SetupCache folder.
rem The exit code is msiexec's (0, or 3010 for a restart that is not needed).
setlocal
set NETKEY=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{92FB6C44-E685-45AD-9B20-CADF4CABA132}
msiexec.exe /x{16735AF7-1D8D-3681-94A5-C578A61EC832} /qn /norestart
set RC=%errorlevel%
if not "%RC%"=="0" if not "%RC%"=="3010" exit /b %RC%
reg delete "%NETKEY%" /f >nul 2>&1
reg delete "%NETKEY%.KB4503575" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Updates\Microsoft .NET Framework 4.8" /f >nul 2>&1
rd /s /q "%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\SetupCache" >nul 2>&1
rd /s /q "%SystemRoot%\Microsoft.NET\Framework\v4.0.30319\SetupCache" >nul 2>&1
exit /b %RC%
