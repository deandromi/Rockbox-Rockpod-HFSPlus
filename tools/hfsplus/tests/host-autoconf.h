/* SPDX-License-Identifier: GPL-2.0-or-later */
/* This fixture supports host checks only and must never build firmware. */
#ifndef __BUILD_AUTOCONF_H
#define __BUILD_AUTOCONF_H
#define arch_none 0
#define arch_arm64 1
#define arch_m68k 2
#define arch_arm 3
#define arch_mips 4
#define arch_x86 5
#define arch_amd64 6
#define ARCH_NONE arch_none
#define ARCH_ARM64 arch_arm64
#define ARCH_M68K arch_m68k
#define ARCH_ARM arch_arm
#define ARCH_MIPS arch_mips
#define ARCH_X86 arch_x86
#define ARCH_AMD64 arch_amd64
#define ARM_PROFILE_CLASSIC 0
#define ARM_PROFILE_MICRO 1
#define ARM_PROFILE_APPLICATION 2
#define ARCH arch_arm
#define ARCH_VERSION 5
#define ARCH_PROFILE ARM_PROFILE_CLASSIC
#define ROCKBOX_LITTLE_ENDIAN 1
#define GCCNUM 905
#define ASSEMBLER_THREADS
#define ROCKBOX_DIR "/.rockbox"
#define ROCKBOX_SHARE_PATH ""
#define ROCKBOX_BINARY_PATH ""
#define ROCKBOX_LIBRARY_PATH ""
#endif
