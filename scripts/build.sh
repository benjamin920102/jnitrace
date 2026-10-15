#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${JAVA_HOME:?Set JAVA_HOME to a JDK}"
mkdir -p src/main/resources/native
# Build both original supported x64 platforms from an Ubuntu runner.
g++ -std=c++17 -O2 -shared -fPIC -pthread -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" native/jnitrace.cpp -ldl -o src/main/resources/native/libjnitrace.so
bash build-dll.sh
mvn -B --no-transfer-progress clean package
