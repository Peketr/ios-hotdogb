@echo off
cd /d "%~dp0"
set "ADB=C:\Program Files\platform-tools\adb.exe"
set "FB=C:\Program Files\platform-tools\fastboot.exe"
echo === %date% %time% > ud.log
"%FB%" devices | findstr fastboot >nul && ("%FB%" reboot >> ud.log 2>&1)
echo waiting for Android (adb)...
"%ADB%" wait-for-device
:boot
"%ADB%" shell getprop sys.boot_completed 2>nul | findstr 1 >nul || (timeout /t 2 >nul & goto boot)
"%ADB%" push ud.sh /data/local/tmp/ud.sh >> ud.log 2>&1
"%ADB%" shell "su -c 'sh /data/local/tmp/ud.sh'" >> ud.log 2>&1
if exist ud rmdir /s /q ud
"%ADB%" pull /data/local/tmp/ud ud >> ud.log 2>&1
echo === done >> ud.log
type ud.log
pause
