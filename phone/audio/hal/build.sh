#!/bin/sh
# Build SweetAudio.driver (the HAL plug-in iOS plays into) for iOS 16 arm64 into phone/audio/hal/out, signed ad hoc.
# Install on the phone: tools/hotdogb-audio-install.py (needs the kernel file server).
set -e
cd "$(dirname "$0")"
D=out/SweetAudio.driver
rm -rf out && mkdir -p $D
../../../work/userland/bin/ios-cc -O2 -Wall -bundle -o $D/SweetAudio sweet_audio.c -framework CoreFoundation
cp Info.plist $D/Info.plist
codesign -f -s - $D 2>/dev/null
ls -la $D
