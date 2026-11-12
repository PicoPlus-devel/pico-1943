:
# ====================================================================================
# pico-1943 build all script
# Builds pico1943 for every supported hardware configuration, in Release.
# Binaries are copied to the releases folder.
#
# RP2350 only: every config is built with -2 (the build refuses RP2040, so
# HW_CONFIG 3 and 4 are not built; 11 is deprecated). PSRAM is optional (boards
# without it run the game from a copy in flash) and both video back-ends are
# built: HSTX on 2, 5, 8, 13, 14 and 15, PicoDVI on 1, 6, 7, 9, 10 and 12.
# ====================================================================================
cd `dirname $0` || exit 1
[ -d releases ] && rm -rf releases
mkdir releases || exit 1
# check picotool exists in path
if ! command -v picotool &> /dev/null
then
	echo "picotool could not be found"
	echo "Please install picotool from https://github.com/raspberrypi/picotool.git"
	exit 1
fi
HWCONFIGS="1 2 5 6 7 8 9 10 12 13 14 15 17 18"
for HWCONFIG in $HWCONFIGS
do
	./bld.sh -c $HWCONFIG -2 || exit 1
done
if [ -z "$(ls -A releases)" ]; then
	echo "No UF2 files found in releases folder"
	exit 1
fi
for UF2 in releases/*.uf2
do
	ls -l $UF2
	picotool info $UF2
	echo " "
done
