#!/bin/bash
# build the PowerPC peripheral tests (Mac OS X 10.6 + Xcode 3.2.x with the PowerPC compiler)
cd "$(dirname "$0")"
F="-arch ppc -O1 -mmacosx-version-min=10.4 -Wno-deprecated-declarations"
gcc $F -o usb_list usb_list.c -framework IOKit -framework CoreFoundation || exit 1
gcc $F -o audio_out audio_out.c -framework CoreAudio -framework AudioUnit -framework AudioToolbox -framework Carbon || exit 1
gcc $F -o audio_in audio_in.c -framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework Carbon || exit 1
gcc $F -o webcam webcam.c -framework QuickTime -framework Carbon || exit 1
gcc $F -o serial serial.c -framework IOKit -framework CoreFoundation || exit 1
gcc $F -o qt_movie qt_movie.c -framework QuickTime -framework Carbon || exit 1
gcc $F -o qt_play qt_play.c -framework QuickTime -framework Carbon || exit 1
gcc $F -o threads threads.c -framework Carbon || exit 1
gcc $F -o midi midi.c -framework CoreMIDI -framework CoreFoundation || exit 1
gcc $F -mmacosx-version-min=10.5 -o services services.c -framework Security -framework CoreServices -framework CoreFoundation -framework SystemConfiguration -framework IOKit -framework DiskArbitration || exit 1
gcc $F -mmacosx-version-min=10.5 -o hid hid.c -framework IOKit -framework CoreFoundation || exit 1
gcc $F -o hidold hidold.c -framework IOKit -framework CoreFoundation || exit 1
lipo -info hid hidold usb_list audio_out audio_in webcam serial qt_movie qt_play threads midi services
