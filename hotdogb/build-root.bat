@echo off
rem Build the iOS root image (HFS+, 16 GB) inside WSL. Asks for your WSL sudo password.
echo Log: %~dp0build-root.log
wsl.exe bash /mnt/c/Users/Peketr/ios-flame/hotdogb/build-root.sh
pause
