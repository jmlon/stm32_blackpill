#!/bin/sh
# Install LEAF (https://github.com/spiricom/LEAF) as an Arduino library.
#
# LEAF is not in the Arduino library index and ships no library.properties,
# so arduino-cli cannot install it.  This fetches a pinned commit and lays
# out its C sources in the Arduino 1.5 library format:
#
#     <libraries>/LEAF/library.properties
#     <libraries>/LEAF/src/{leaf.h,leaf-config.h,Inc/,Src/,Externals/}
#
# leaf/leaf.cpp is left out on purpose: it is a unity build that #includes
# every Src/*.c, and the Arduino builder already compiles those one by one.
#
# The default commit is from 2026-03-09.  The next one upstream (the
# 2026-07-15 "basic-phisem" merge) leaves Src/leaf-physical.c unbuildable.
#
# Usage: tools/install-leaf.sh [commit]   (libraries dir: $ARDUINO_LIBRARIES,
#                                          default ~/Arduino/libraries)
set -eu

rev=${1:-751f3c83e2da57adfde7142aec2bcd2282deecb3}
libraries=${ARDUINO_LIBRARIES:-$HOME/Arduino/libraries}
dest=$libraries/LEAF

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

git clone --quiet https://github.com/spiricom/LEAF.git "$tmp/LEAF"
git -C "$tmp/LEAF" -c advice.detachedHead=false checkout --quiet "$rev"

rm -rf "$dest"
mkdir -p "$dest/src"
cp -r "$tmp/LEAF/leaf/leaf.h" "$tmp/LEAF/leaf/leaf-config.h" \
      "$tmp/LEAF/leaf/Inc" "$tmp/LEAF/leaf/Src" "$tmp/LEAF/leaf/Externals" \
      "$dest/src/"
cp "$tmp/LEAF/LICENSE" "$tmp/LEAF/README.md" "$dest/"

cat > "$dest/library.properties" <<EOF
name=LEAF
version=0.0.0
author=Jeff Snyder, Mike Mulshine, Matt Wang
maintainer=Jeff Snyder
sentence=Lightweight Embedded Audio Framework (commit $rev).
paragraph=Oscillators, filters, envelopes, delays, reverbs and physical models in C, for 32-bit ARM microcontrollers.
category=Signal Input/Output
url=https://github.com/spiricom/LEAF
architectures=*
includes=leaf.h
EOF

echo "Installed LEAF $rev in $dest"
