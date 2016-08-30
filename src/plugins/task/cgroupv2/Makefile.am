# Makefile for task/cgroupv2 plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common $(HWLOC_CPPFLAGS)

pkglib_LTLIBRARIES = task_cgroupv2.la

# cgroupv2 task plugin.
task_cgroupv2_la_SOURCES = 	task_cgroupv2.h task_cgroupv2.c \
				task_cgroupv2_memory.h task_cgroupv2_memory.c \
				task_cgroupv2_blkio.h task_cgroupv2_blkio.c
task_cgroupv2_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS)
