#!/bin/sh
# Copyright 2026, RenkuOS contributors.
# Distributed under the terms of the MIT License.
#
# Makes APFS test images on macOS for apfs_shell. The images are raw
# containers (no partition map) and stay out of the repository.
#
#   make_test_images.sh <output folder>
#
# plain.dmg   64 MB, one volume "Plain": files, a deep directory, a symlink.
#             Expect: 3 files, 2 directories, 1 symlink.
# multi.dmg   1.5 GB, volumes "Main", "Second", "CaseVol" (case-sensitive).
#             Expect: volume list in this order; macOS adds .fseventsd files
#             to every volume it mounts, so file counts include those.

set -e

out=${1:?usage: $0 <output folder>}
mkdir -p "$out"
cd "$out"

attach() {
	hdiutil attach -nobrowse "$1" | awk 'NR == 1 { print $1 }'
}

hdiutil create -size 64m -fs APFS -volname Plain -layout NONE plain.dmg
disk=$(attach plain.dmg)
mkdir -p /Volumes/Plain/docs/sub
echo hello > /Volumes/Plain/hello.txt
seq 1 20000 > /Volumes/Plain/docs/numbers.txt
ln -s hello.txt /Volumes/Plain/link
echo deep > /Volumes/Plain/docs/sub/deep.txt
sync
hdiutil detach "$disk"

hdiutil create -size 1536m -fs APFS -volname Main -layout NONE multi.dmg
disk=$(attach multi.dmg)
diskutil apfs addVolume "$disk" APFS Second
diskutil apfs addVolume "$disk" "Case-sensitive APFS" CaseVol
echo main > /Volumes/Main/main.txt
echo two > /Volumes/Second/two.txt
sync
hdiutil detach "$disk"
