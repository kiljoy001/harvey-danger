#!/bin/sh
# Fetch the Phase 0 reference sources into upstream/ (gitignored).
set -eu
cd "$(dirname "$0")/.."
mkdir -p upstream

kernel_commit=e90d994cb5fe2531ff35ee13fb624f4bba2c07cc
if [ ! -f upstream/linux-sc8830.tar.gz ]; then
	curl -L -o upstream/linux-sc8830.tar.gz \
		"https://github.com/pstglia/linux/archive/$kernel_commit.tar.gz"
fi
if [ ! -d upstream/linux ]; then
	mkdir -p upstream/linux
	tar -xzf upstream/linux-sc8830.tar.gz -C upstream/linux --strip-components=1
fi

if [ ! -f upstream/dt.img ]; then
	curl -L -o upstream/dt.img \
		"https://raw.githubusercontent.com/sixito007/android_device_samsung_goyave3g/cm-14.1/dt.img"
fi

if [ ! -f upstream/config-samsung-goyavewifi.armv7 ]; then
	curl -L -o upstream/config-samsung-goyavewifi.armv7 \
		"https://raw.githubusercontent.com/kuskebabi/apkbuild-samsung-goyavewifi/main/linux-samsung-goyavewifi/config-samsung-goyavewifi.armv7"
fi

echo "sources in upstream/"
