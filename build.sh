#!/bin/bash

rm -rf build
mkdir build
clang ./dump-header-map.c -o ./build/dump-header-map_x86_64 -target x86_64-apple-macos
clang ./dump-header-map.c -o ./build/dump-header-map_arm64 -target arm64-apple-macos
lipo -create ./build/dump-header-map_x86_64 ./build/dump-header-map_arm64 -o ./build/dump-header-map

pushd build
tar -cjf dump-header-map.tar.gz dump-header-map
popd
