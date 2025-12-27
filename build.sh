#!/usr/bin/env bash
set -eo pipefail

cmake -B profiler/build -S profiler -DCMAKE_BUILD_TYPE=Release

cmake --build profiler/build --config Release --parallel

sudo install -Dm755 profiler/build/tracy-profiler /usr/bin/tracy
