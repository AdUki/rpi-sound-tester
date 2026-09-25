# Sound Tester
#
#   make            list the targets
#   make run        run it on this machine against a simulated card
#   make image      build the read-only Yocto image (BOARD=rpi3 or vim3l)
#   make flash      write it to an SD card

SHELL := /bin/bash
.DEFAULT_GOAL := help

APP    := app
BUILD  := $(APP)/build
BIN    := $(BUILD)/soundtesterd

# The ALSA plugin runs on the machine that SENDS audio — a laptop, a build box, anything on the
# bench — not on the Pi, so it is built for this host and is deliberately not in the image.
PLUGIN       := alsa-plugin
PLUGIN_BUILD := $(PLUGIN)/build
PLUGIN_SO    := $(PLUGIN_BUILD)/libasound_module_pcm_soundtester.so
# VORBIS=0 builds the plugin without the encoder, and without any libvorbis dependency.
VORBIS  ?=

# Plain poky + bitbake. No kas, no pip: the layers are git clones and the build dir's conf files
# are generated. BOARD picks the hardware; yocto/boards/$(BOARD).mk says what that means for the
# build (MACHINE, the daemon's board profile, the BSP layer), and yocto/conf/boards/$(BOARD).conf
# holds the board's bitbake settings. Each board builds in its own dir; downloads and sstate are
# shared through site.conf.
BOARD   ?= rpi3
YOCTO   := yocto
LAYERS  := $(YOCTO)/layers
BOARDS  := $(patsubst $(YOCTO)/boards/%.mk,%,$(wildcard $(YOCTO)/boards/*.mk))
ifeq ($(filter $(BOARD),$(BOARDS)),)
  $(error BOARD=$(BOARD) is not one of: $(BOARDS))
endif
include $(YOCTO)/boards/$(BOARD).mk
BOARD_MK   := $(YOCTO)/boards/$(BOARD).mk
BOARD_CONF := $(YOCTO)/conf/boards/$(BOARD).conf
YB      := $(YOCTO)/build-$(BOARD)
BRANCH  := scarthgap
DEPLOY  := $(YB)/tmp/deploy/images/$(MACHINE)
# MACHINE is exported as well as written into auto.conf: oe-init-build-env passes it through to
# bitbake, and auto.conf's hard assignment is what holds once the build is running.
BB       = set -e && export MACHINE=$(MACHINE) && \
           . $(LAYERS)/poky/oe-init-build-env $(CURDIR)/$(YB) >/dev/null &&
# The generated build config every bitbake invocation needs.
BUILD_CONF = check-builddir $(YB)/conf/bblayers.conf $(YB)/conf/auto.conf build-localconf $(SITE_LINK)

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
# Extra outputs beyond the sound cards the daemon finds by itself, e.g. this machine's speakers
# through its sound server (switch it on in the console):
#   make run SINK=default
SINK    ?=
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
run: build ## Run it: simulated card by default, or DEVICE=hw:... for a real one; SINK=dev adds an output (e.g. default)
	@mkdir -p /tmp/soundtester
ifeq ($(DEVICE),)
	@echo -e "$(BOLD)http://localhost:$(PORT)$(OFF)  $(DIM)simulated card, each channel delayed $(STAGGER) frames$(OFF)"
	@$(BIN) --sim --sim-stagger $(STAGGER) --port $(PORT) --board $(APP)/config/boards/$(PROFILE).json \
	        $(foreach d,$(SINK),--sink $(d)) \
	        --www $(APP)/www --config $(APP)/config/default-config.json --data-dir /tmp/soundtester
else
	@$(BIN) --device $(DEVICE) --port $(PORT) --board $(APP)/config/boards/$(PROFILE).json \
	        $(foreach d,$(SINK),--sink $(d)) \
	        --www $(APP)/www --config $(APP)/config/default-config.json --data-dir /tmp/soundtester
endif

.PHONY: plugin
plugin: ## Build the ALSA plugin for THIS machine (VORBIS=0 to leave out the encoder)
	@cmake -S $(PLUGIN) -B $(PLUGIN_BUILD) \
	       -DST_VORBIS=$(if $(filter 0,$(VORBIS)),OFF,ON) >/dev/null
	@cmake --build $(PLUGIN_BUILD) -j$$(nproc)
	@echo -e "$(BOLD)$(PLUGIN_SO)$(OFF)"
	@echo -e "$(DIM)Try it without installing anything:$(OFF)"
	@echo -e "  ALSA_PLUGIN_DIR=$(CURDIR)/$(PLUGIN_BUILD) aplay -D soundtester:soundtester.local x.wav"

.PHONY: plugin-install
plugin-install: plugin ## Install the plugin and its .conf system-wide (needs sudo)
	@set -e; 	  plugdir=$$(pkg-config --variable=libdir alsa)/alsa-lib; 	  echo "installing into $$plugdir and /usr/share/alsa/alsa.conf.d"; 	  sudo install -d "$$plugdir" /usr/share/alsa/alsa.conf.d /etc/alsa/conf.d; 	  sudo install -m 0755 $(PLUGIN_SO) "$$plugdir/"; 	  sudo ln -sf libasound_module_pcm_soundtester.so "$$plugdir/libasound_module_ctl_soundtester.so"; 	  sudo install -m 0644 $(PLUGIN)/50-soundtester.conf /usr/share/alsa/alsa.conf.d/; 	  sudo ln -sf /usr/share/alsa/alsa.conf.d/50-soundtester.conf /etc/alsa/conf.d/50-soundtester.conf
	@echo -e "$(BOLD)aplay -D soundtester:soundtester.local x.wav$(OFF)"

.PHONY: plugin-uninstall
plugin-uninstall: ## Remove the installed plugin and its .conf (needs sudo)
	@set -e; 	  plugdir=$$(pkg-config --variable=libdir alsa)/alsa-lib; 	  sudo rm -f "$$plugdir/libasound_module_pcm_soundtester.so" 	             "$$plugdir/libasound_module_ctl_soundtester.so" 	             /usr/share/alsa/alsa.conf.d/50-soundtester.conf 	             /etc/alsa/conf.d/50-soundtester.conf
	@echo "removed"

.PHONY: clean
clean: ## Remove the local build (add FULL=1 to also drop the Yocto build tree)
	@# $(BUILD)-vec is the by-hand tree for vectorization reports:
	@#   cmake -S $(APP) -B $(BUILD)-vec -DST_VECTORIZE_REPORT=ON
	@rm -rf $(BUILD) $(BUILD)-vec $(PLUGIN_BUILD)
ifdef FULL
	@rm -rf $(YB)/tmp
	@echo "Yocto tmp removed; downloads/ and sstate-cache/ kept, so a rebuild is much faster."
endif

## ─── deploy ──────────────────────────────────────────────────────────────────

# Push a local change to a running board over ssh, without a reflash. The rootfs is a
# writable ext4 mounted read-only, so both targets remount it rw, write, sync, then remount
# ro again (the change persists on the SD card). Override the ssh destination with TARGET=.
#   make deploy-www                              # to root@soundtester.local
#   make deploy-daemon TARGET=root@192.168.1.42
TARGET    ?= root@soundtester.local
WWW_DEST  := /usr/share/soundtester/www
BIN_DEST  := /usr/bin/soundtesterd
# The stripped binary from `make bitbake ARGS="soundtesterd"`, found in the recipe's PKGDEST;
# the libraries it may need are looked up under the board's TUNE_PKGARCH. Both come from
# bitbake, so the target works for any board's toolchain. The daemon reads its www from disk per
# request, so a frontend swap needs no restart; a binary swap does.
yocto_vars = $(BB) bitbake -e soundtesterd | sed -n 's/^\(PKGDEST\|TUNE_PKGARCH\)="\(.*\)"$$/\1=\2/p'

.PHONY: deploy-www
deploy-www: ## Copy app/www to a running board over ssh (no restart; TARGET=root@host)
	@echo -e "$(BOLD)app/www → $(TARGET):$(WWW_DEST)$(OFF)"
	@ssh $(TARGET) 'mount -o remount,rw /'
	@scp -r $(APP)/www/* $(TARGET):$(WWW_DEST)/
	@ssh $(TARGET) 'sync && mount -o remount,ro /'
	@echo -e "Done. Hard-refresh the browser $(DIM)(Ctrl+Shift+R)$(OFF)."

.PHONY: deploy-daemon
deploy-daemon: $(BUILD_CONF) ## Copy the cross-compiled daemon, its /etc files and app/www to a board, restart it (TARGET=root@host)
	@set -e; \
	eval "$$($(yocto_vars))"; \
	bin="$$PKGDEST/soundtesterd/usr/bin/soundtesterd"; \
	if [ ! -e "$$bin" ]; then \
	  echo -e "$(BOLD)No cross-compiled daemon.$(OFF) $$bin is missing."; \
	  echo -e "Build it first:  $(BOLD)make bitbake ARGS=\"soundtesterd\" BOARD=$(BOARD)$(OFF)"; exit 1; fi; \
	echo -e "$(BOLD)$$bin → $(TARGET):$(BIN_DEST)$(OFF)"; \
	ssh $(TARGET) 'systemctl stop soundtesterd'; \
	ssh $(TARGET) 'mount -o remount,rw /'; \
	scp "$$bin" $(TARGET):$(BIN_DEST); \
	: 'Its board and factory config files ride along: a new daemon may read keys an older'; \
	: 'image did not ship.'; \
	scp -q "$$PKGDEST"/soundtesterd/etc/soundtester/* $(TARGET):/etc/soundtester/; \
	: 'A new DEPENDS reaches the board only through a reflash, so a binary that has grown a'; \
	: 'library since the image was built would land here and then refuse to start. Carry over'; \
	: 'anything it needs that the board has not got; a reflash installs them properly.'; \
	for lib in $$(readelf -d "$$bin" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do \
	  if ! ssh $(TARGET) "test -e /usr/lib/$$lib -o -e /lib/$$lib" 2>/dev/null; then \
	    src=$$(find $(YB)/tmp/sysroots-components -name "$$lib" -path "*/$$TUNE_PKGARCH/*" | head -1); \
	    if [ -n "$$src" ]; then \
	      echo -e "  $(DIM)+ $$lib (missing on the board)$(OFF)"; \
	      scp -q "$$(readlink -f $$src)" $(TARGET):/usr/lib/$$lib; \
	    else \
	      echo -e "  $(BOLD)! $$lib is missing on the board and not in the sysroot$(OFF)"; \
	    fi; \
	  fi; \
	done; \
	: 'The console and the daemon change together, so the console goes too.'; \
	scp -rq $(APP)/www/* $(TARGET):$(WWW_DEST)/; \
	ssh $(TARGET) 'sync && mount -o remount,ro /'; \
	ssh $(TARGET) 'systemctl start soundtesterd'; \
	echo "Done. Daemon and console restarted with the new build."

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

# The newest image of this board, whatever its BSP calls it: meta-raspberrypi's names carry
# ".rootfs", meta-meson empties IMAGE_NAME_SUFFIX and so its names do not.
newest_image = ls -t $(DEPLOY)/$(IMAGE)-$(MACHINE)*.wic.bz2 2>/dev/null | head -1

.PHONY: image
image: check-host check-submodules check-devconf $(BUILD_CONF) ## Build the image (BOARD=rpi3|vim3l, DEV=1 for the dev image)
	@$(BB) bitbake $(IMAGE)
	@echo ""
	@IMG=$$($(newest_image)); \
	if [ -n "$$IMG" ]; then \
	  echo -e "$(BOLD)$$(du -h $$(readlink -f $$IMG) | cut -f1)$(OFF)  $$IMG"; \
	  echo -e "  flash it:  $(BOLD)make flash BOARD=$(BOARD)$(if $(DEV), DEV=1) DISK=<device>$(OFF)"; \
	fi

.PHONY: bitbake
bitbake: check-host check-devconf $(BUILD_CONF) ## Run bitbake with the layers set up (ARGS=..., or no ARGS for a shell)
ifeq ($(ARGS),)
	@echo "Layers configured for BOARD=$(BOARD) (MACHINE=$(MACHINE)). 'exit' to leave."
	@$(BB) $$SHELL
else
	@$(BB) bitbake $(ARGS)
endif

# The layers and build config: git clones, and conf files generated per board.
$(LAYERS)/poky:
	@echo "Cloning poky ($(BRANCH))..."
	@git clone -q --depth 1 -b $(BRANCH) https://git.yoctoproject.org/poky $@

$(LAYERS)/meta-openembedded:
	@echo "Cloning meta-openembedded ($(BRANCH))..."
	@git clone -q --depth 1 -b $(BRANCH) https://git.openembedded.org/meta-openembedded $@

$(LAYERS)/meta-raspberrypi:
	@echo "Cloning meta-raspberrypi ($(BRANCH))..."
	@git clone -q --depth 1 -b $(BRANCH) https://git.yoctoproject.org/meta-raspberrypi $@

# meta-meson develops on master; its scarthgap branch is frozen, so pin the commit it froze at.
MESON_REV := 524ab0409b54ebed5d113d4251a9561d7798cb45

$(LAYERS)/meta-meson:
	@echo "Cloning meta-meson ($(BRANCH), $(shell echo $(MESON_REV) | cut -c1-8))..."
	@git clone -q -b $(BRANCH) https://github.com/superna9999/meta-meson $@
	@git -C $@ checkout -q $(MESON_REV)

$(YB)/conf/bblayers.conf: $(LAYERS)/poky $(LAYERS)/meta-openembedded $(BSP_LAYERS) \
                          $(YOCTO)/conf/bblayers.conf.sample $(BOARD_MK)
	@mkdir -p $(YB)/conf
	@printf '  %s \\\n' \
	  $(CURDIR)/$(LAYERS)/poky/meta \
	  $(CURDIR)/$(LAYERS)/poky/meta-poky \
	  $(CURDIR)/$(LAYERS)/meta-openembedded/meta-oe \
	  $(CURDIR)/$(LAYERS)/meta-openembedded/meta-python \
	  $(CURDIR)/$(LAYERS)/meta-openembedded/meta-networking \
	  $(CURDIR)/$(LAYERS)/meta-openembedded/meta-multimedia \
	  $(addprefix $(CURDIR)/,$(BSP_LAYERS)) \
	  $(CURDIR)/$(YOCTO)/meta-soundtester > $(YB)/conf/.layers
	@sed -e '/@LAYERS@/{r $(YB)/conf/.layers' -e 'd}' \
	     $(YOCTO)/conf/bblayers.conf.sample > $(YB)/conf/bblayers.conf
	@rm -f $(YB)/conf/.layers
	@echo "Layers for BOARD=$(BOARD) written to $(YB)/conf/bblayers.conf."

# What makes this build dir this board's. bitbake reads auto.conf before local.conf, and the
# board conf is required by absolute path rather than copied, so an edit to it is picked up (and
# tracked by bitbake) without regenerating anything here.
$(YB)/conf/auto.conf: $(BOARD_MK) Makefile
	@mkdir -p $(YB)/conf
	@{ echo "# Written by the Makefile for BOARD=$(BOARD) from $(BOARD_MK). Do not edit:"; \
	   echo "# it is regenerated. One-off settings go in local.conf."; \
	   echo 'MACHINE = "$(MACHINE)"'; \
	   echo 'SOUNDTESTER_BOARD = "$(PROFILE)"'; \
	   echo 'require $(CURDIR)/$(BOARD_CONF)'; } > $@

# local.conf is yours to edit, so it is only ever created, never rewritten — except that one
# from before the board split (it carries the Pi's settings, which now live in the board conf and
# would otherwise be applied twice) is moved aside for the current sample, which is marked.
LOCALCONF_MARK := soundtester-local-conf: board-neutral

.PHONY: build-localconf
build-localconf: $(YB)/conf/bblayers.conf
	@if [ -f $(YB)/conf/local.conf ] && ! grep -q '$(LOCALCONF_MARK)' $(YB)/conf/local.conf; then \
	  mv $(YB)/conf/local.conf $(YB)/conf/local.conf.pre-split; \
	  echo -e "$(BOLD)Note:$(OFF) $(YB)/conf/local.conf predates the per-board config and was moved to"; \
	  echo    "      local.conf.pre-split. Carry any one-off change you made in it over by hand."; \
	fi
	@if [ ! -f $(YB)/conf/local.conf ]; then \
	  cp $(YOCTO)/conf/local.conf.sample $(YB)/conf/local.conf; \
	  echo "Build config written to $(YB)/conf/ (local.conf is yours to edit)."; \
	fi

# Before the split there was a single yocto/build. Moving it keeps its sstate-backed state, but
# bitbake refuses a build dir whose tmp/ has moved (tmp/saved_tmpdir), so tmp goes: the next build
# repopulates it from sstate-cache, which lives outside the build dir.
.PHONY: check-builddir
check-builddir:
	@if [ -d $(YOCTO)/build/conf ]; then \
	  echo -e "$(BOLD)yocto/build is from before the per-board build dirs.$(OFF) Move it once:"; \
	  echo -e "\n  $(BOLD)mv yocto/build yocto/build-rpi3 && rm -rf yocto/build-rpi3/tmp$(OFF)\n"; \
	  echo    "Downloads and sstate are outside it, so the next build restores tmp/ from sstate."; \
	  exit 1; fi

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
flash: ## Write the image to a disk (DISK=/dev/..., BOARD=, DEV=1 for the dev image)
	@IMG=$$($(newest_image)); \
	if [ -z "$$IMG" ]; then echo "No image. Run 'make image BOARD=$(BOARD)$(if $(DEV), DEV=1)' first."; exit 1; fi; \
	ROOTDISK="/dev/$$($(rootdisk))"; \
	if [ -z "$(DISK)" ]; then \
	  echo -e "Usage: $(BOLD)make flash BOARD=$(BOARD)$(if $(DEV), DEV=1) DISK=<device>$(OFF)   e.g. /dev/sdb, /dev/mmcblk0, /dev/sda\n"; \
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
	echo -e "\n$(BOLD)Done.$(OFF) Boot the board and open http://soundtester.local"

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
	@if grep -qE '^SOUNDTESTER_(RATE|PERIOD)' $(DEVCONF); then \
	  echo -e "$(DIM)Note: SOUNDTESTER_RATE/PERIOD in $(DEVCONF) no longer take effect: the board"; \
	  echo -e "conf ($(BOARD_CONF)) owns them. Override in $(YB)/conf/local.conf instead.$(OFF)"; fi

.PHONY: help
help:
	@echo -e "$(BOLD)Sound Tester$(OFF)  $(DIM)boards: $(BOARDS) (BOARD=$(BOARD))$(OFF)\n"
	@awk 'BEGIN {FS = ":.*## "} \
	     /^## ─/ { gsub(/## /,""); printf "\n\033[2m%s\033[0m\n", $$0; next } \
	     /^[a-zA-Z_-]+:.*?## / { printf "  \033[1m%-11s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo -e "\n$(DIM)Flags:  BOARD=rpi3|vim3l  DEV=1 (dev image)  FULL=1 (deeper clean)  ARGS=\"...\" (bitbake)$(OFF)"
	@echo -e "$(DIM)Vars:   DISK=/dev/...  DEVICE=hw:...  PORT=$(PORT)  TARGET=root@host$(OFF)\n"
