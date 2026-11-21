# GNU Make on Windows delegates to the same tested MSVC/NMAKE rules.
SHELL := cmd.exe
.SHELLFLAGS := /d /s /c
NMAKE ?= nmake
.PHONY: all input telemetry check test-effects test-telemetry check-hardware
all input telemetry check test-effects test-telemetry check-hardware:
	$(NMAKE) /nologo /f Makefile.msvc $@
