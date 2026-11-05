@echo off
rem Fetch the parts of the iPhone X iOS 16.7.16 IPSW that the iOS root needs, inside WSL.
echo Fetching from Apple's CDN into WSL ~/ios-flame-ipsw (several GB, resumable).
echo Log: %~dp0ipsw.log
wsl.exe bash /mnt/c/Users/Peketr/ios-flame/hotdogb/fetch-ipsw.sh
pause
