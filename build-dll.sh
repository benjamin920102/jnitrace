#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${JAVA_HOME:?Set JAVA_HOME to a JDK}"
mkdir -p src/main/resources/native
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ -pthread \
  -Inative/win32 -I"$JAVA_HOME/include" native/jnitrace.cpp -o src/main/resources/native/jnitrace.dll
