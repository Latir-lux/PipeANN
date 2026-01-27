#!/bin/bash
set -e

mkdir -p build
cd build

cmake_args=(
  ..
  -DUSE_AIO=ON
  -DCMAKE_EXPORT_COMPILE_COMMANDS=1
)

if [ -x "/home/xiaoxuanx/bin/gcc" ] && [ -x "/home/xiaoxuanx/bin/g++" ]; then
  cmake_args+=(
    -DCMAKE_C_COMPILER=/home/xiaoxuanx/bin/gcc
    -DCMAKE_CXX_COMPILER=/home/xiaoxuanx/bin/g++
  )
fi

cmake "${cmake_args[@]}"
make -j
make compare_systems gen_tags
