#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${JAVA_HOME:?Set JAVA_HOME to a JDK}"
bash scripts/build-libffi.sh
mkdir -p src/main/resources/native
"${MINGW_CXX:-x86_64-w64-mingw32-g++-posix}" -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ -pthread \
  -Inative/win32 -I"$JAVA_HOME/include" -Itarget/libffi-win/include native/jnitrace.cpp \
  target/libffi-win/lib/libffi.a -o src/main/resources/native/jnitrace.dll
