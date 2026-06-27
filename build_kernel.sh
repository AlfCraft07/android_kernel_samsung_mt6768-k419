#!/bin/bash

export TC=/home/vigus/zyc-clang

export CROSS_COMPILE=$TC/bin/aarch64-linux-gnu-
export LD=$TC/bin/ld.lld
export OBJCOPY=$TC/bin/llvm-objcopy
export AS=$TC/bin/llvm-as
export NM=$TC/bin/llvm-nm
export STRIP=$TC/bin/llvm-strip
export OBJDUMP=$TC/bin/llvm-objdump
export READELF=$TC/bin/llvm-readelf
export CC=$TC/bin/clang
export CLANG_TRIPLE=aarch64-linux-gnu-
export ARCH=arm64
#export ANDROID_MAJOR_VERSION=r

export KCFLAGS=-w
export CONFIG_SECTION_MISMATCH_WARN_ONLY=y

make -C $(pwd) O=$(pwd)/out KCFLAGS=-w CONFIG_SECTION_MISMATCH_WARN_ONLY=y LLVM=1 clean && make -C $(pwd) O=$(pwd)/out KCFLAGS=-w CONFIG_SECTION_MISMATCH_WARN_ONLY=y LLVM=1 mrproper
clear

make -s -C $(pwd) O=$(pwd)/out KCFLAGS=-w CONFIG_SECTION_MISMATCH_WARN_ONLY=y LLVM=1 a13ve_defconfig
make -s -C $(pwd) O=$(pwd)/out KCFLAGS=-w CONFIG_SECTION_MISMATCH_WARN_ONLY=y LLVM=1 -j$(nproc)

