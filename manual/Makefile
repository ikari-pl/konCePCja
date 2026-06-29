# konCePCja User Manual — Typst build (English edition)
#
# Forked from the sibling CPC6128 manual toolchain. Strategy A: free fonts only
# (Rokkitt / TeX Gyre Schola / Skala OCR-B + OFL/CC-BY-SA set) committed under
# fonts/, so the build is reproducible anywhere (and in CI).

# Resolve the fonts dir from the Makefile's own location, not the shell CWD, so
# `make -C manual` from any directory still finds the fonts (no silent fallback).
MKFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

PDF   = koncepcja_manual.pdf
HTML  = koncepcja_manual.html
SRC   = manual_build.typ
FONTS = $(MKFILE_DIR)fonts
SITE  = $(MKFILE_DIR)site

.PHONY: all html site watch clean

all: $(PDF)

$(PDF): $(SRC) template.typ i18n.typ $(wildcard chapters/*.typ) $(wildcard images/*)
	typst compile --font-path $(FONTS) $(SRC) $(PDF)

# Web build (experimental Typst HTML export + web/style.css theme). The HTML and
# its theme reference the fonts under fonts/, so serve fonts/ alongside the page.
html: $(HTML)

$(HTML): $(SRC) template.typ i18n.typ web/style.css $(wildcard chapters/*.typ)
	typst compile --features html --font-path $(FONTS) --format html $(SRC) $(HTML)

# Assemble a flat, deployable directory for GitHub Pages:
#   site/index.html            — the homepage (../ asset paths rewritten to flat)
#   site/koncepcja_manual.html — the manual (images inlined as data URIs; needs fonts/)
#   site/fonts/  site/images/  — assets referenced by the two pages
site: $(HTML)
	rm -rf $(SITE)
	mkdir -p $(SITE)/fonts $(SITE)/images
	cp $(HTML) $(SITE)/koncepcja_manual.html
	sed -e 's#\.\./fonts/#fonts/#g' -e 's#\.\./images/#images/#g' -e 's#\.\./koncepcja_manual\.html#koncepcja_manual.html#g' web/index.html > $(SITE)/index.html
	cp fonts/*.ttf fonts/*.otf $(SITE)/fonts/ 2>/dev/null || true
	cp images/*.png images/*.svg $(SITE)/images/ 2>/dev/null || true
	touch $(SITE)/.nojekyll
	@echo "Built $(SITE)/ — open $(SITE)/index.html, or deploy the directory to GitHub Pages."

# Rebuild on every save.
watch:
	typst watch --font-path $(FONTS) $(SRC) $(PDF)

clean:
	rm -rf $(PDF) $(HTML) $(SITE)
