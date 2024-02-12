#!/bin/sh
#
# Build script for the Atari build
# Charlotte Koch <dressupgeekout@gmail.com>
#

set -ex

MAKE=${MAKE:-make}
TRIPLE=${TRIPLE:-m68k-atari-mint}

${MAKE} build/janet -j4 \
  CC=${TRIPLE}-gcc \
  AR=${TRIPLE}-ar \
  RANLIB=${TRIPLE}-ranlib \
  STRIP=${TRIPLE}-strip
  HOSTCC=${TRIPLE}-gcc \
  HOSTAR=${TRIPLE}-ar \
  HOSTRANLIB=${TRIPLE}-ranlib \
  HOSTSTRIP=${TRIPLE}-strip
