#
# Copyright (c) 2015-2019, NVIDIA CORPORATION. All rights reserved.
#
# See LICENSE.txt for license information
#
.PHONY : all clean
# 默认的边缘目标
default : src.build
install : src.install

# build 目录
BUILDDIR ?= $(abspath ./build)
# 当前目录
ABSBUILDDIR := $(abspath $(BUILDDIR))
TARGETS := src pkg
clean: ${TARGETS:%=%.clean}
test.build: src.build
LICENSE_FILES := LICENSE.txt
LICENSE_TARGETS := $(LICENSE_FILES:%=$(BUILDDIR)/%)
lic: $(LICENSE_TARGETS)

${BUILDDIR}/%.txt: %.txt
	@printf "Copying    %-35s > %s\n" $< $@
	mkdir -p ${BUILDDIR}
	cp $< $@

# 编译src.* 目标的编译规则：编译src目录 make -C src build $(BUILDDIR)
src.%:
	${MAKE} -C src $* BUILDDIR=${ABSBUILDDIR}

# 出包
pkg.%:
	${MAKE} -C pkg $* BUILDDIR=${ABSBUILDDIR}

pkg.debian.prep: lic
pkg.txz.prep: lic
