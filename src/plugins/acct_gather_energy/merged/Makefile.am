# Makefile for acct_gather_energy/merged plugin

AUTOMAKE_OPTIONS = foreign

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common $(FREEIPMI_CPPFLAGS)

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

pkglib_LTLIBRARIES = acct_gather_energy_merged.la

# merged energy accounting plugin.
acct_gather_energy_merged_la_SOURCES = acct_gather_energy_merged.c acct_gather_energy_ipmi_config.c acct_gather_energy_ipmi_config.h

acct_gather_energy_merged_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS) $(FREEIPMI_LDFLAGS)
acct_gather_energy_merged_la_LIBADD  = -lm -lnvidia-ml $(FREEIPMI_LIBS)
