# RPi Sound Tester
#
#   make            list the targets
#   make run        run it on this machine against a simulated card
#   make image      build the read-only Yocto image
#   make flash      write it to an SD card

SHELL := /bin/bash
.DEFAULT_GOAL := help

APP    := app
BUILD  := $(APP)/build
BIN    := $(BUILD)/soundtesterd

# Plain poky + bitbake. No kas, no pip: the layers are three git clones and two conf files.
YOCTO   := yocto
LAYERS  := $(YOCTO)/layers
YB      := $(YOCTO)/build
BRANCH  := scarthgap
MACHINE := raspberrypi3
DEPLOY  := $(YB)/tmp/deploy/images/$(MACHINE)
BB       = set -e && . $(LAYERS)/poky/oe-init-build-env $(CURDIR)/$(YB) >/dev/null &&

# DEV=1 selects the writable development image (alsa-utils, ssh, package manager) instead
# of the read-only production one. Used by both `image` and `flash`.
DEV     ?=
IMAGE    = soundtester-image$(if $(DEV),-dev)

PORT    ?= 8080
# `make run` simulates the card. Give a real one to drive hardware:
#   make run DEVICE=hw:audioinjectoroc,0
DEVICE  ?=
# Simulator: input channel c is fed from output c, delayed by c*STAGGER frames, so every
# channel pair has a known delay to measure.
STAGGER ?= 137
DISK    ?=

# What `make configure` writes. Neither is tracked by git.
#   DEVCONF  — baked into the image (hostname, ssh password, Wi-Fi). Copied from the tracked
#              .sample on first use: it carries a root password and a Wi-Fi PSK in the clear,
#              and git keeps whatever it is given.
#   SITECONF — where this machine keeps its Yocto caches. Per-machine, and bitbake parses
#              conf/site.conf before conf/local.conf, so a ?= here wins over the defaults in
#              local.conf.sample without anyone having to edit a generated file.
DEVCONF  := $(YOCTO)/meta-soundtester/conf/soundtester-device.conf
SITECONF := $(YOCTO)/conf/site.conf
# Only a build prerequisite once `make configure` has actually produced a site.conf.
SITE_LINK  = $(if $(wildcard $(SITECONF)),$(YB)/conf/site.conf)

# The factory defaults `make configure` offers, and the only place they are written down.
# They cannot be read back out of the conf files: those hold the values in force *now*, not
# the defaults.
DEF_HOSTNAME     := soundtester
DEF_PASSWORD     := soundtester
DEF_SSH          := 1
DEF_WIFI_SSID    :=
DEF_WIFI_PSK     :=
DEF_WIFI_COUNTRY := SK
DEF_DL_DIR       := $(CURDIR)/$(YOCTO)/downloads
DEF_SSTATE_DIR   := $(CURDIR)/$(YOCTO)/sstate-cache

BOLD := \033[1m
DIM  := \033[2m
OFF  := \033[0m

## ─── develop ─────────────────────────────────────────────────────────────────

.PHONY: build
build: check-submodules $(BUILD)/CMakeCache.txt ## Compile the daemon for this machine
	@cmake --build $(BUILD) -j$$(nproc)

$(BUILD)/CMakeCache.txt:
	@cmake -S $(APP) -B $(BUILD) -DCMAKE_BUILD_TYPE=Release

# The libraries are git submodules (header-only, pinned). A clone without --recurse-submodules
# leaves them as empty directories, and the first sign of it would otherwise be a missing header.
.PHONY: check-submodules
check-submodules:
	@if [ ! -e $(APP)/third_party/httplib/httplib.h ]; then \
	  echo -e "$(BOLD)Submodules not checked out.$(OFF) The third-party libraries are pinned as git"; \
	  echo    "submodules; app/third_party/* is empty."; \
	  echo -e "\n  $(BOLD)git submodule update --init$(OFF)\n"; \
	  exit 1; fi

.PHONY: test
test: build ## Run the test suite
	@ctest --test-dir $(BUILD) --output-on-failure

.PHONY: run
run: build ## Run it: simulated card by default, or DEVICE=hw:... for a real one
	@mkdir -p /tmp/soundtester
ifeq ($(DEVICE),)
	@echo -e "$(BOLD)http://localhost:$(PORT)$(OFF)  $(DIM)simulated card, each channel delayed $(STAGGER) frames$(OFF)"
	@$(BIN) --sim --sim-stagger $(STAGGER) --port $(PORT) \
	        --www $(APP)/www --config $(APP)/config/default-config.json --data-dir /tmp/soundtester
else
	@$(BIN) --device $(DEVICE) --port $(PORT) \
	        --www $(APP)/www --config $(APP)/config/default-config.json --data-dir /tmp/soundtester
endif

.PHONY: clean
clean: ## Remove the local build (add FULL=1 to also drop the Yocto build tree)
	@# $(BUILD)-vec is the by-hand tree for vectorization reports:
	@#   cmake -S $(APP) -B $(BUILD)-vec -DST_VECTORIZE_REPORT=ON
	@rm -rf $(BUILD) $(BUILD)-vec
ifdef FULL
	@rm -rf $(YB)/tmp
	@echo "Yocto tmp removed; downloads/ and sstate-cache/ kept, so a rebuild is much faster."
endif

## ─── configure ───────────────────────────────────────────────────────────────

# Everything here can equally well be done by editing the two files by hand; this target just
# means you do not have to know which setting lives in which of them.
.PHONY: configure
configure: ## Set hostname, ssh password, Wi-Fi and the Yocto cache dirs (interactive)
	@set -e; \
	[ -f $(DEVCONF) ] || cp $(DEVCONF).sample $(DEVCONF); \
	get() { sed -n "s|^$$2 *?\?= *\"\(.*\)\"|\1|p" "$$1" 2>/dev/null | head -1; }; \
	: 'ask LABEL DEFAULT CURRENT -> ANS. The bracket shows the factory DEFAULT, and the line'; \
	: 'comes up pre-filled with the value in force now, which you can edit or erase. Clearing'; \
	: 'it and pressing Enter restores the default — otherwise there is no way back to it once'; \
	: 'a value has been written, since the conf file it was read from is the same file we are'; \
	: 'about to overwrite. Every default is empty-safe: WIFI_SSID/WIFI_PSK default to empty,'; \
	: 'so "erase to reset" and "erase to disable Wi-Fi" are the same gesture.'; \
	ask() { \
	  printf "\n  $(BOLD)%s$(OFF)\n  $(DIM)default: %s$(OFF)\n" "$$1" "$${2:-<empty>}" >&2; \
	  read -e -i "$$3" -r -p "  > " ANS || ANS=""; \
	  [ -n "$$ANS" ] || ANS="$$2"; \
	  case "$$ANS" in '~'/*) ANS="$$HOME$${ANS#\~}";; esac; \
	  case "$$ANS" in *'"'*) echo -e "\n  A double quote cannot go in a bitbake value. Edit the file by hand." >&2; exit 1;; esac; \
	}; \
	setkey() { \
	  f="$$1"; export K="$$2" V="$$3"; \
	  awk 'BEGIN { k = ENVIRON["K"]; v = ENVIRON["V"]; hit = 0 } \
	       index($$0, k) == 1 && substr($$0, length(k) + 1) ~ /^[ \t]*\??=/ { \
	         print k " ?= \"" v "\""; hit = 1; next } \
	       { print } \
	       END { if (!hit) print k " ?= \"" v "\"" }' "$$f" > "$$f.new" && mv "$$f.new" "$$f"; \
	}; \
	echo -e "\n$(DIM)The line is pre-filled with the current value: edit it, or clear it and press$(OFF)"; \
	echo -e "$(DIM)Enter to restore the default shown above it.$(OFF)"; \
	echo -e "\n$(BOLD)Device$(OFF) $(DIM)— baked into the image; the rootfs is read-only, so these cannot be changed on a running board.$(OFF)"; \
	ask "Hostname (the board answers on http://<hostname>.local)" "$(DEF_HOSTNAME)" "$$(get $(DEVCONF) SOUNDTESTER_HOSTNAME)"; host="$$ANS"; \
	ask "Root password (for ssh; stored in the clear in $(DEVCONF))" "$(DEF_PASSWORD)" "$$(get $(DEVCONF) SOUNDTESTER_ROOT_PASSWORD)"; pass="$$ANS"; \
	ask "Enable ssh? (1/0)" "$(DEF_SSH)" "$$(get $(DEVCONF) SOUNDTESTER_ENABLE_SSH)"; ssh="$$ANS"; \
	echo -e "\n$(BOLD)Wi-Fi$(OFF) $(DIM)— an empty SSID builds an Ethernet-only image (no wpa_supplicant, no firmware blob, radio off).$(OFF)"; \
	ask "Wi-Fi SSID" "$(DEF_WIFI_SSID)" "$$(get $(DEVCONF) SOUNDTESTER_WIFI_SSID)"; ssid="$$ANS"; \
	psk=""; country="$$(get $(DEVCONF) SOUNDTESTER_WIFI_COUNTRY)"; \
	if [ -n "$$ssid" ]; then \
	  ask "Wi-Fi password" "$(DEF_WIFI_PSK)" "$$(get $(DEVCONF) SOUNDTESTER_WIFI_PSK)"; psk="$$ANS"; \
	  ask "Wi-Fi country (2-letter regulatory domain)" "$(DEF_WIFI_COUNTRY)" "$$country"; country="$$ANS"; \
	fi; \
	echo -e "\n$(BOLD)Build host$(OFF) $(DIM)— caches. Repo-local by default; point them at a shared tree (e.g. ~/yocto/downloads) and every project reuses one.$(OFF)"; \
	ask "Yocto download dir (DL_DIR)" "$(DEF_DL_DIR)" "$$(get $(SITECONF) DL_DIR)"; dl="$$ANS"; \
	ask "Yocto sstate cache (SSTATE_DIR)" "$(DEF_SSTATE_DIR)" "$$(get $(SITECONF) SSTATE_DIR)"; ss="$$ANS"; \
	setkey $(DEVCONF) SOUNDTESTER_HOSTNAME      "$$host"; \
	setkey $(DEVCONF) SOUNDTESTER_ROOT_PASSWORD "$$pass"; \
	setkey $(DEVCONF) SOUNDTESTER_ENABLE_SSH    "$$ssh"; \
	setkey $(DEVCONF) SOUNDTESTER_WIFI_SSID     "$$ssid"; \
	setkey $(DEVCONF) SOUNDTESTER_WIFI_PSK      "$$psk"; \
	setkey $(DEVCONF) SOUNDTESTER_WIFI_COUNTRY  "$$country"; \
	mkdir -p $$(dirname $(SITECONF)) "$$dl" "$$ss"; \
	{ echo "# Written by 'make configure'. Per-machine, not tracked by git."; \
	  echo "# bitbake parses conf/site.conf before conf/local.conf, so these win over the"; \
	  echo "# defaults in yocto/conf/local.conf.sample."; \
	  echo ""; } > $(SITECONF); \
	setkey $(SITECONF) DL_DIR     "$$dl"; \
	setkey $(SITECONF) SSTATE_DIR "$$ss"; \
	echo -e "\n$(BOLD)Written$(OFF)"; \
	echo "  $(DEVCONF)"; \
	echo "      hostname=$$host  ssh=$$ssh  wifi=$${ssid:-<none>}"; \
	echo "  $(SITECONF)"; \
	echo "      DL_DIR=$$dl"; \
	echo "      SSTATE_DIR=$$ss"; \
	echo -e "\nNext: $(BOLD)make image$(OFF)\n"

## ─── image ───────────────────────────────────────────────────────────────────

.PHONY: image
image: check-host check-submodules check-devconf $(YB)/conf/bblayers.conf $(SITE_LINK) ## Build the image (DEV=1 for the dev image)
	@$(BB) bitbake $(IMAGE)
	@echo ""
	@IMG=$(DEPLOY)/$(IMAGE)-$(MACHINE).rootfs.wic.bz2; \
	if [ -e "$$IMG" ]; then \
	  echo -e "$(BOLD)$$(du -h $$(readlink -f $$IMG) | cut -f1)$(OFF)  $$IMG"; \
	  echo -e "  flash it:  $(BOLD)make flash$(if $(DEV), DEV=1) DISK=<device>$(OFF)"; \
	fi

.PHONY: bitbake
bitbake: check-host check-devconf $(YB)/conf/bblayers.conf $(SITE_LINK) ## Run bitbake with the layers set up (ARGS=..., or no ARGS for a shell)
ifeq ($(ARGS),)
	@echo "Layers configured. 'exit' to leave."
	@$(BB) $$SHELL
else
	@$(BB) bitbake $(ARGS)
endif

# The layers and build config: three clones and two generated conf files.
$(LAYERS)/poky:
	@echo "Cloning poky ($(BRANCH))..."
	@git clone -q --depth 1 -b $(BRANCH) https://git.yoctoproject.org/poky $@

$(LAYERS)/meta-openembedded:
	@echo "Cloning meta-openembedded ($(BRANCH))..."
	@git clone -q --depth 1 -b $(BRANCH) https://git.openembedded.org/meta-openembedded $@

$(LAYERS)/meta-raspberrypi:
	@echo "Cloning meta-raspberrypi ($(BRANCH))..."
	@git clone -q --depth 1 -b $(BRANCH) https://git.yoctoproject.org/meta-raspberrypi $@

$(YB)/conf/bblayers.conf: $(LAYERS)/poky $(LAYERS)/meta-openembedded $(LAYERS)/meta-raspberrypi \
                          $(YOCTO)/conf/bblayers.conf.sample $(YOCTO)/conf/local.conf.sample
	@mkdir -p $(YB)/conf
	@printf '  %s \\\n' \
	  $(CURDIR)/$(LAYERS)/poky/meta \
	  $(CURDIR)/$(LAYERS)/poky/meta-poky \
	  $(CURDIR)/$(LAYERS)/meta-openembedded/meta-oe \
	  $(CURDIR)/$(LAYERS)/meta-openembedded/meta-python \
	  $(CURDIR)/$(LAYERS)/meta-openembedded/meta-networking \
	  $(CURDIR)/$(LAYERS)/meta-openembedded/meta-multimedia \
	  $(CURDIR)/$(LAYERS)/meta-raspberrypi \
	  $(CURDIR)/$(YOCTO)/meta-soundtester > $(YB)/conf/.layers
	@sed -e '/@LAYERS@/{r $(YB)/conf/.layers' -e 'd}' \
	     $(YOCTO)/conf/bblayers.conf.sample > $(YB)/conf/bblayers.conf
	@rm -f $(YB)/conf/.layers
	@[ -f $(YB)/conf/local.conf ] || cp $(YOCTO)/conf/local.conf.sample $(YB)/conf/local.conf
	@echo "Build config written to $(YB)/conf/ (local.conf is yours to edit)."

# Copied rather than symlinked so that a `make clean FULL=1` cannot take the original with it.
$(YB)/conf/site.conf: $(SITECONF) $(YB)/conf/bblayers.conf
	@cp $(SITECONF) $@

## ─── flash ───────────────────────────────────────────────────────────────────

# Lists the disks you could plausibly write to: every whole disk that is not the one this
# system is running from. Deliberately NOT filtered on the "removable" flag — a card in a
# built-in reader (/dev/mmcblk0) reports RM=0, and filtering on it would hide the most common
# way people flash an SD card.
# (--exclude 7 drops loop devices.) Expects $$ROOTDISK to be set by the caller.
candidates = lsblk -dno NAME,SIZE,TRAN,MODEL --exclude 7 \
	     | grep -v "^$$(basename $$ROOTDISK) " | sed 's|^|  /dev/|'
# The disk holding "/" — the one we must never write to.
rootdisk = lsblk -no PKNAME "$$(findmnt -no SOURCE /)" 2>/dev/null | head -1

.PHONY: flash
flash: ## Write the image to a disk (DISK=/dev/..., DEV=1 for the dev image)
	@IMG=$$(ls -t $(DEPLOY)/$(IMAGE)-$(MACHINE).rootfs*.wic.bz2 2>/dev/null | head -1); \
	if [ -z "$$IMG" ]; then echo "No image. Run 'make image$(if $(DEV), DEV=1)' first."; exit 1; fi; \
	ROOTDISK="/dev/$$($(rootdisk))"; \
	if [ -z "$(DISK)" ]; then \
	  echo -e "Usage: $(BOLD)make flash DISK=<device>$(OFF)   e.g. /dev/sdb, /dev/mmcblk0, /dev/sda\n"; \
	  echo "Disks on this machine (the system disk $$ROOTDISK is not listed):"; \
	  $(candidates); exit 1; fi; \
	if [ ! -b "$(DISK)" ]; then echo "$(DISK) is not a block device."; exit 1; fi; \
	if [ "$$(lsblk -dno TYPE $(DISK) 2>/dev/null | tr -d ' ')" != "disk" ]; then \
	  PARENT=$$(lsblk -no PKNAME $(DISK) 2>/dev/null | head -1); \
	  echo "$(DISK) is a partition, not a whole disk.$${PARENT:+ Did you mean /dev/$$PARENT?}"; exit 1; fi; \
	if [ "$(DISK)" = "$$ROOTDISK" ]; then \
	  echo "REFUSING: $(DISK) is the disk this system is running from."; exit 1; fi; \
	if lsblk -lno MOUNTPOINT $(DISK) | grep -qE '^(/|/boot|/boot/.*|/usr|/var|/etc|/home)$$'; then \
	  echo "REFUSING: $(DISK) carries a mounted system directory:"; \
	  lsblk -lno NAME,MOUNTPOINT $(DISK) | grep -E ' /'; exit 1; fi; \
	echo -e "$(BOLD)About to ERASE $(DISK) — everything on it is lost:$(OFF)"; \
	lsblk -o NAME,SIZE,TRAN,RM,MODEL,MOUNTPOINT $(DISK); \
	if [ "$$(lsblk -dno RM $(DISK) | tr -d ' ')" != "1" ]; then \
	  echo -e "\n  $(BOLD)WARNING: this disk is not flagged removable.$(OFF) That is normal for a card in a"; \
	  echo    "  built-in reader (/dev/mmcblk0), but it is also what an internal drive looks like."; \
	  echo    "  Check the size and model above before you continue."; \
	fi; \
	echo -e "\n  image: $$IMG"; \
	read -p $$'\nType YES to write it: ' ans; [ "$$ans" = "YES" ] || { echo "Cancelled."; exit 1; }; \
	for p in $$(lsblk -lno NAME,MOUNTPOINT $(DISK) | awk '$$2 != "" {print "/dev/"$$1}'); do \
	  echo "unmounting $$p"; sudo umount "$$p" || exit 1; \
	done; \
	BMAP=$${IMG%.wic.bz2}.wic.bmap; \
	if command -v bmaptool >/dev/null && [ -e "$$BMAP" ]; then \
	  sudo bmaptool copy --bmap "$$BMAP" "$$IMG" $(DISK); \
	else \
	  bunzip2 -c "$$IMG" | sudo dd of=$(DISK) bs=4M conv=fsync status=progress; \
	fi; \
	sync; \
	echo -e "\n$(BOLD)Done.$(OFF) Boot the Pi and open http://soundtester.local"

## ─── setup ───────────────────────────────────────────────────────────────────

HOST_PKGS := gawk wget git diffstat unzip texinfo gcc build-essential chrpath socat cpio \
             python3 python3-pexpect xz-utils debianutils iputils-ping python3-git \
             python3-jinja2 python3-subunit zstd liblz4-tool file locales libacl1 bmap-tools

.PHONY: host-deps
host-deps: ## Install the host packages Yocto needs (Debian/Ubuntu)
	@missing=""; for p in $(HOST_PKGS); do dpkg -s $$p >/dev/null 2>&1 || missing="$$missing $$p"; done; \
	if [ -z "$$missing" ]; then echo "All host dependencies already installed."; \
	else echo -e "Installing:$$missing\n"; sudo apt-get install -y $$missing; fi

.PHONY: check-host
check-host:
	@missing=""; for p in $(HOST_PKGS); do dpkg -s $$p >/dev/null 2>&1 || missing="$$missing $$p"; done; \
	if [ -n "$$missing" ]; then \
	  echo -e "Missing host packages:$$missing\nInstall them with:  $(BOLD)make host-deps$(OFF)"; exit 1; fi

# The layer `require`s DEVCONF, so a fresh clone would otherwise fail deep inside a bitbake
# parse. Say what is missing and why it is not in the repo instead.
.PHONY: check-devconf
check-devconf:
	@if [ ! -f $(DEVCONF) ]; then \
	  echo -e "$(BOLD)No device config.$(OFF) $(DEVCONF) is not tracked by git: it holds"; \
	  echo    "the root password and Wi-Fi PSK in the clear."; \
	  echo -e "\n  $(BOLD)make configure$(OFF)   set hostname, password and Wi-Fi (starts from the defaults)"; \
	  echo    "  or copy $$(basename $(DEVCONF)).sample over it and edit by hand"; \
	  exit 1; fi

.PHONY: help
help:
	@echo -e "$(BOLD)RPi Sound Tester$(OFF)\n"
	@awk 'BEGIN {FS = ":.*## "} \
	     /^## ─/ { gsub(/## /,""); printf "\n\033[2m%s\033[0m\n", $$0; next } \
	     /^[a-zA-Z_-]+:.*?## / { printf "  \033[1m%-11s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo -e "\n$(DIM)Flags:  DEV=1 (dev image)  FULL=1 (deeper clean)  ARGS=\"...\" (bitbake)$(OFF)"
	@echo -e "$(DIM)Vars:   DISK=/dev/...  DEVICE=hw:...  PORT=$(PORT)$(OFF)\n"
