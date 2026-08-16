PLUGIN_ID := io.github.raphaelbarreiros.opendeck-galleon.sdPlugin
PLUGIN_ROOT := build/$(PLUGIN_ID)
ARCHIVE := dist/opendeck-galleon-linux-x86_64.zip
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror
HIDAPI_FLAGS := $(shell pkg-config --cflags --libs hidapi-hidraw)

.PHONY: all check clean package

all: bin/galleon-hid

bin/galleon-hid: src/galleon-hid.c
	mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $< $(HIDAPI_FLAGS)

check: bin/galleon-hid
	node --check src/main.mjs
	jq --exit-status . manifest.json pt_BR.json >/dev/null
	./bin/galleon-hid --help >/dev/null 2>&1 || test $$? -eq 1

package: check
	rm -rf build dist
	mkdir -p $(PLUGIN_ROOT)/assets $(PLUGIN_ROOT)/bin $(PLUGIN_ROOT)/docs $(PLUGIN_ROOT)/packaging $(PLUGIN_ROOT)/src dist
	cp manifest.json pt_BR.json README.md LICENSE.md $(PLUGIN_ROOT)/
	cp assets/galleon-background.png assets/plugin.png $(PLUGIN_ROOT)/assets/
	cp bin/galleon-hid $(PLUGIN_ROOT)/bin/
	cp docs/protocol.md $(PLUGIN_ROOT)/docs/
	cp packaging/70-opendeck-galleon.rules $(PLUGIN_ROOT)/packaging/
	cp src/main.mjs $(PLUGIN_ROOT)/src/
	cp packaging/70-opendeck-galleon.rules dist/
	cd build && bsdtar -a -cf ../$(ARCHIVE) $(PLUGIN_ID)

clean:
	rm -rf bin build dist
