#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${JAVA_HOME:?Set JAVA_HOME to a JDK}"
mkdir -p src/main/resources/native
g++ -std=c++17 -O2 -shared -fPIC -pthread -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
  native/jnitrace.cpp $(pkg-config --cflags --libs libffi) -ldl -o src/main/resources/native/libjnitrace.so
bash build-dll.sh
# Maven clean would remove the libffi build cache; packaging alone is sufficient.
mvn -B --no-transfer-progress package
