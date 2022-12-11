#!/bin/sh
#
# Build script for the Atari build
# Charlotte Koch <dressupgeekout@gmail.com>
#

set -ex

MAKE=${MAKE:-make}
TRIPLE=${TRIPLE:-m68k-atari-mint}

${MAKE} build/c/janet.c -j4 \
  HOSTCC=${TRIPLE}-gcc \
  HOSTAR=${TRIPLE}-ar \
  HOSTRANLIB=${TRIPLE}-ranlib \
  HOSTSTRIP=${TRIPLE}-strip
