# LEADOUT - build the DOS client and the DOS tools.
# Works on Ubuntu (and Debian) and on CachyOS (and Arch).
#
# First time:
#   make deps                      install the system packages (uses sudo)
#   make setup                     download OpenWatcom 2 (about 150 MB)
#
# Then:
#   make                           build LEADOUT.COM and the DOS folder into dist/
#   make release                   make dist/leadout-$(VERSION).zip
#   make clean                     remove dist/ and the build files
#   make distclean                 also remove the downloaded tools
#
# Use an OpenWatcom that you have already:
#   make WATCOM=~/openwatcom

SHELL := /bin/bash
.DEFAULT_GOAL := all

# The version: VERSION_STR in src/leadout.h. LEADOUT shows it at start.
VERSION     ?= $(shell sed -n 's/.*VERSION_STR *"\([^"]*\)".*/\1/p' src/leadout.h)

TOOLS       ?= $(HOME)/.leadout-tools
WATCOM      ?= $(TOOLS)/openwatcom
WATCOM_URL  := https://github.com/open-watcom/open-watcom-v2/releases/download/Current-build/ow-snapshot.tar.xz

DIST        := dist
PKG         := $(DIST)/LEADOUT

APT_PKGS    := nasm zip unzip curl xz-utils
PACMAN_PKGS := nasm zip unzip curl xz make

.PHONY: all help deps setup watcom check-watcom dos release clean distclean

all: dos
	@echo
	@echo "Done. The DOS folder is in $(PKG)/"

help:
	@sed -n '2,15p' $(firstword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'

deps:
	@. /etc/os-release; \
	case " $$ID $$ID_LIKE " in \
	  *" arch "*|*" cachyos "*) sudo pacman -S --needed --noconfirm $(PACMAN_PKGS) ;; \
	  *" ubuntu "*|*" debian "*) sudo apt-get update && sudo apt-get install -y $(APT_PKGS) ;; \
	  *) echo "This Makefile knows Ubuntu, Debian, CachyOS and Arch. Your system: $$NAME"; \
	     echo "Install these packages yourself: $(APT_PKGS)"; exit 1 ;; \
	esac

setup: watcom
	@echo "Tools ready in $(TOOLS). Now run: make"

watcom: $(WATCOM)/binl64/wcc
$(WATCOM)/binl64/wcc:
	@echo "Downloading OpenWatcom 2 to $(WATCOM)"
	mkdir -p "$(WATCOM)"
	curl -fL --retry 3 -o "$(TOOLS)/ow-snapshot.tar.xz" $(WATCOM_URL)
	tar -xJf "$(TOOLS)/ow-snapshot.tar.xz" -C "$(WATCOM)"
	rm -f "$(TOOLS)/ow-snapshot.tar.xz"
	@test -x $@ || (echo "OpenWatcom: binl64/wcc not found after the download"; exit 1)

check-watcom:
	@test -x "$(WATCOM)/binl64/wcc" || (echo "OpenWatcom not found in $(WATCOM). Run: make setup"; exit 1)
	@command -v nasm > /dev/null || (echo "NASM not found. Run: make deps"; exit 1)

dos: check-watcom
	export WATCOM="$(WATCOM)" PATH="$(WATCOM)/binl64:$$PATH" INCLUDE="$(WATCOM)/h" && sh src/build.sh
	nasm -f bin -o tools/CDPLAY.COM tools/cdplay.asm
	nasm -f bin -o tools/CDSPEED.COM tools/cdspeed.asm
	mkdir -p $(PKG)
	cp src/LEADOUT.COM tools/CDPLAY.COM tools/CDSPEED.COM package/* $(PKG)/

release: dos
	rm -f $(DIST)/leadout-$(VERSION).zip
	cp README.md PROTOCOL.md $(DIST)/
	cd $(DIST) && zip -qr leadout-$(VERSION).zip README.md PROTOCOL.md LEADOUT
	@echo "Release: $(DIST)/leadout-$(VERSION).zip"

clean:
	rm -rf $(DIST) src/*.obj src/leadout.map src/LEADOUT.COM tools/CDPLAY.COM tools/CDSPEED.COM

distclean: clean
	rm -rf "$(TOOLS)"
