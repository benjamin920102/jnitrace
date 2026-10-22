#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
archive="$PWD/third_party/libffi-3.4.6.tar.gz"
expected=b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e
actual=$(sha256sum "$archive" | cut -d' ' -f1)
[[ "$actual" == "$expected" ]] || { echo "libffi checksum mismatch" >&2; exit 1; }
prefix="$PWD/target/libffi-win"
[[ -f "$prefix/lib/libffi.a" ]] && exit 0
mkdir -p target/libffi-src target/libffi-build
tar --no-same-owner -xzf "$archive" -C target/libffi-src --strip-components=1
cd target/libffi-build
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc-posix}" ../libffi-src/configure --host=x86_64-w64-mingw32 --disable-shared --enable-static --prefix="$prefix"
make -j"${BUILD_JOBS:-2}"
make install
