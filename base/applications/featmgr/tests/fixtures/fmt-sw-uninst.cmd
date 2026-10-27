@echo off
del /q %SystemRoot%\Featmgr\fmt-sw.installed
reg delete HKLM\SOFTWARE\ReactOS\Nitro\FmTest /v Sw /f >nul
exit 0
