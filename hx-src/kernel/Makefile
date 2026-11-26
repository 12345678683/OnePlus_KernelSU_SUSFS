# SPDX-License-Identifier: GPL-2.0
#
# HX (幻形) 内核模块
#
# 目标：Android GKI 6.6 arm64（PJZ110 / Android 17 实测环境）
#
# 编译（推荐 NDK r27 + 对应 KMI 的内核源码树）：
#   export KDIR=/path/to/android15-6.6/out/.../    # 含 Module.symvers 的 build 树
#   make -C $KDIR M=$PWD ARCH=arm64 LLVM=1 modules
#
# 也可以直接把本目录挂进内核树编成 obj-y（见 partition/build_in_kernel.sh）

obj-m += hx.o

hx-y := hx_main.o hx_mem.o hx_scan.o hx_drm.o hx_touch.o hx_hide.o hx_gate.o

# DRM 内部头文件不在 include/ 下，必须自己加进来
ccflags-y += -I$(srctree)/drivers/gpu/drm

# 关掉一堆会暴露结构的调试信息
ccflags-y += -fno-asynchronous-unwind-tables -fno-unwind-tables
ccflags-y += -fvisibility=hidden
ccflags-y += -O2 -Wno-unused-function -Wno-unused-variable

KBUILD_CFLAGS_MODULE += -fno-pic

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 LLVM=1 modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
