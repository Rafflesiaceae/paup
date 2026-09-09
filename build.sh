#!/usr/bin/env bash
set -eo pipefail

# Reconfigure an existing tree so meson.build edits are always reflected.
if [[ -f build/meson-private/coredata.dat ]]; then
	meson setup --reconfigure build
else
	meson setup build
fi

meson compile -C build
