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
# stress.dmg  256 MB, volume "Stress", made from stress-src/: 3000 files in
#             one directory (a B-tree of many nodes), a 20 MB random file, a
#             sparse file, an empty file, NFC and NFD spellings of one name,
#             Japanese, emoji and 251+4 character names, a 6-level path; in
#             the image also a hard link (data-link.txt) and a compressed
#             file (compressed.txt, ditto --hfsCompression).
#             Check: copy /myfs out with apfs_shell's "cp -r" and diff it
#             against stress-src/ (compressed.txt cannot be read yet).
# attrs.dmg   64 MB, volume "Attrs": tagged.txt with an embedded attribute
#             (user.small, 11 bytes) and a stream-backed one (user.big,
#             10000 bytes, over XATTR_MAX_EMBEDDED_SIZE), a symlink whose
#             target attribute must stay hidden, and, if SYSINFO names a
#             RenkuOS x86_64 binary, that program (to run it from APFS).

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

src=stress-src
rm -rf "$src"
mkdir -p "$src/many" "$src/a/b/c/d/e/f"
for i in $(seq -w 0 2999); do echo "file $i" > "$src/many/f$i"; done
head -c 20971520 /dev/urandom > "$src/big.bin"
dd if=/dev/urandom of="$src/sparse.bin" bs=4096 count=1 seek=2560 2>/dev/null
: > "$src/empty.txt"
echo composed > "$src/$(printf 'caf\xc3\xa9-nfc.txt')"
echo decomposed > "$src/$(printf 'cafe\xcc\x81-nfd.txt')"
echo nihongo > "$src/$(printf '\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt')"
echo emoji > "$src/$(printf 'emoji-\xf0\x9f\x98\x80.txt')"
echo long > "$src/$(printf 'L%.0s' $(seq 1 251)).txt"
echo deep > "$src/a/b/c/d/e/f/deep.txt"
echo shared > "$src/data.txt"

hdiutil create -size 256m -fs APFS -volname Stress -layout NONE stress.dmg
disk=$(attach stress.dmg)
cp -R "$src/" /Volumes/Stress/
ln /Volumes/Stress/data.txt /Volumes/Stress/data-link.txt
seq 1 300000 > compress-me.txt
ditto --hfsCompression compress-me.txt /Volumes/Stress/compressed.txt
rm compress-me.txt
sync
hdiutil detach "$disk"

hdiutil create -size 64m -fs APFS -volname Attrs -layout NONE attrs.dmg
disk=$(attach attrs.dmg)
echo hello > /Volumes/Attrs/tagged.txt
xattr -w user.small "short value" /Volumes/Attrs/tagged.txt
xattr -w user.big "$(python3 -c "print('x' * 9999, end='Z')")" \
	/Volumes/Attrs/tagged.txt
ln -s tagged.txt /Volumes/Attrs/link
if [ -n "$SYSINFO" ]; then
	cp "$SYSINFO" /Volumes/Attrs/sysinfo
	chmod 755 /Volumes/Attrs/sysinfo
	xattr -c /Volumes/Attrs/sysinfo
fi
sync
hdiutil detach "$disk"
