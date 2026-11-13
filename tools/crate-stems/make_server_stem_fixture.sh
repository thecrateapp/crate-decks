#!/bin/sh
# The server stem file fixture of src/test/crate/stemsync_test.cpp (ST5.PR2):
# an MP4 with three AAC streams, drums, bass and vocals, written the way the
# Crate server writes its stem files (crate.smart_mix.stem_files, STEM-D6), at a
# lower bitrate to keep it small. Each stem is one tone of the original that the
# test generates: 0.3 sin(60 Hz) + 0.3 sin(110 Hz) + 0.2 sin(440 Hz), 2 s at
# 44.1 kHz.
set -eu
out=${1:-src/test/crate/data/stems/server-stems.mp4}
tone() {
    echo "aevalsrc=$1*sin(2*PI*$2*t)|$1*sin(2*PI*$2*t):s=44100:d=2"
}
ffmpeg -nostdin -v error -y \
    -f lavfi -i "$(tone 0.3 60)" \
    -f lavfi -i "$(tone 0.3 110)" \
    -f lavfi -i "$(tone 0.2 440)" \
    -map 0:a -map 1:a -map 2:a \
    -c:a aac -b:a 64k \
    -metadata:s:a:0 handler_name=drums \
    -metadata:s:a:1 handler_name=bass \
    -metadata:s:a:2 handler_name=vocals \
    -map_metadata -1 -fflags +bitexact -flags:a +bitexact \
    -movflags +faststart -f mp4 "$out"
