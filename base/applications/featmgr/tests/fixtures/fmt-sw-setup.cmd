@echo off
echo installed %1> %SystemRoot%\Featmgr\fmt-sw.installed
reg add HKLM\SOFTWARE\ReactOS\Nitro\FmTest /v Sw /t REG_DWORD /d 1 /f >nul
exit 0
