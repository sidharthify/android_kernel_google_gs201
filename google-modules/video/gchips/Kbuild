# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(srctree)/$(src)/include

obj-$(CONFIG_BIGWAVE) += bigwave.o
bigwave-y += bigo.o bigo_pm.o bigo_io.o bigo_of.o bigo_iommu.o bigo_prioq.o
bigwave-$(CONFIG_SLC_PARTITION_MANAGER) += bigo_slc.o
bigwave-$(CONFIG_DEBUG_FS) += bigo_debug.o
