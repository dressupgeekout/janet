#!/bin/sh
#
# Build script for the Atari build
# Charlotte Koch <dressupgeekout@gmail.com>
#

set -ex

MAKE=${MAKE:-make}
TRIPLE=${TRIPLE:-m68k-atari-mint}

${MAKE} HOSTCC=${TRIPLE}-gcc HOSTAR=${TRIPLE}-ar
