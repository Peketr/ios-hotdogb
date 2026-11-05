@echo off
rem Boot the shim, let it reset back into fastboot, boot Android, pull the log from rawdump.
cd /d "%~dp0"
set "ADB=C:\Program Files\platform-tools\adb.exe"
set "FB=C:\Program Files\platform-tools\fastboot.exe"
echo === %date% %time% > run-shim.log
"%ADB%" get-state 2>nul | findstr device >nul && ("%ADB%" reboot bootloader)
echo waiting for fastboot...
:wait1
"%FB%" devices | findstr fastboot >nul || (timeout /t 2 >nul & goto wait1)
"%FB%" boot shim-hotdogb.img >> run-shim.log 2>&1
echo shim running, waiting for it to reset into fastboot...
timeout /t 10 >nul
:wait2
"%FB%" devices | findstr fastboot >nul || (timeout /t 2 >nul & goto wait2)
echo back in fastboot, booting Android...
"%FB%" reboot >> run-shim.log 2>&1
"%ADB%" wait-for-device
:boot
"%ADB%" shell getprop sys.boot_completed 2>nul | findstr 1 >nul || (timeout /t 2 >nul & goto boot)
"%ADB%" push logpull.sh /data/local/tmp/logpull.sh >> run-shim.log 2>&1
"%ADB%" shell "su -c 'sh /data/local/tmp/logpull.sh'" >> run-shim.log 2>&1
if exist shimlog rmdir /s /q shimlog
"%ADB%" pull /data/local/tmp/shimlog shimlog >> run-shim.log 2>&1
echo === done >> run-shim.log
type run-shim.log
pause
