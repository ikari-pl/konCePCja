# konCePCja User Manual — Typst build (English edition)
#
# Forked from the sibling CPC6128 manual toolchain. Strategy A: free fonts only
# (Rokkitt / TeX Gyre Schola / Skala OCR-B + OFL/CC-BY-SA set) committed under
# fonts/, so the build is reproducible anywhere (and in CI).

# Resolve the fonts dir from the Makefile's own location, not the shell CWD, so
# `make -C manual` from any directory still finds the fonts (no silent fallback).
MKFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

PDF   = koncepcja_manual.pdf
SRC   = manual_build.typ
FONTS = $(MKFILE_DIR)fonts

.PHONY: all watch clean

all: $(PDF)

$(PDF): $(SRC) template.typ i18n.typ $(wildcard chapters/*.typ) $(wildcard images/*)
	typst compile --font-path $(FONTS) $(SRC) $(PDF)

# Rebuild on every save.
watch:
	typst watch --font-path $(FONTS) $(SRC) $(PDF)

clean:
	rm -f $(PDF)
