@echo off
cd /d "%~dp0"
set "ADB=C:\Program Files\platform-tools\adb.exe"
set "FB=C:\Program Files\platform-tools\fastboot.exe"
(
echo === %date% %time%
"%ADB%" get-state 2>nul | findstr device >nul && (echo rebooting to bootloader & "%ADB%" reboot bootloader)
echo waiting for fastboot...
) > boot-shim.log 2>&1
type boot-shim.log
:wait
"%FB%" devices | findstr fastboot >nul || (timeout /t 2 >nul & goto wait)
(
"%FB%" getvar product
"%FB%" getvar unlocked
"%FB%" getvar max-download-size
echo === fastboot boot shim-hotdogb.img
"%FB%" boot shim-hotdogb.img
echo === exit %errorlevel%
) >> boot-shim.log 2>&1
type boot-shim.log
echo.
echo Watch the phone screen now. It should show a dark blue screen with text for 20 s.
pause
