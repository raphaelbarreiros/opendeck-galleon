# OpenDeck support for the Corsair Galleon 100 SD

Unofficial Linux device-support plugin that makes the integrated Stream Deck
panel in the Corsair Galleon 100 SD available to
[OpenDeck](https://github.com/nekename/OpenDeck).

The plugin controls only the dedicated Stream Deck USB component
(`1b1c:2b18`). It does not replace the normal keyboard driver or change the
Galleon's onboard keyboard configuration. Regional keyboard layouts are
expected to work when they expose the same Stream Deck USB ID.

## Features

- 12 LCD keys in their physical 3-column by 4-row layout
- two rotary encoders with press and rotation events
- 720 by 384 LCD, exposed as two OpenDeck encoder regions
- LCD/key images, brightness, key presses, encoder events, and display presses
- automatic device discovery and bridge restart
- software-mode keepalive required by the tested firmware

## Status and safety

This project is experimental and is not affiliated with Corsair, Elgato, or
OpenDeck. It has been tested on Linux with firmware `3.06.005` and USB ID
`1b1c:2b18`. It sends only the display/control reports documented in
[`docs/protocol.md`](docs/protocol.md); firmware flashing and keyboard settings
are deliberately out of scope.

The udev rule is restricted to the Galleon's Stream Deck component, so the
regular keyboard interfaces remain under the standard Linux HID drivers.

## Requirements

- OpenDeck 2.14 or newer
- x86_64 Linux
- Node.js 20 or newer on the host
- ImageMagick 7 (`magick` command)
- `hidapi-hidraw`

On Ubuntu/Debian, install the runtime dependencies with:

```sh
sudo apt install nodejs imagemagick libhidapi-hidraw0
```

## Install

1. Download `opendeck-galleon-linux-x86_64.zip` from the latest release.
2. Open OpenDeck's plugin manager and choose **Install from file**.
3. Download `70-opendeck-galleon.rules` from the same release and install it:

   ```sh
   sudo install -Dm644 packaging/70-opendeck-galleon.rules /etc/udev/rules.d/70-opendeck-galleon.rules
   sudo udevadm control --reload-rules
   sudo udevadm trigger --subsystem-match=hidraw
   ```

4. Reconnect the keyboard or restart OpenDeck.

OpenDeck's official Flatpak remains unchanged and updateable. OpenDeck runs the
plugin through host Node.js because hardware access and ImageMagick live outside
the Flatpak sandbox.

## Build

Install a C compiler, `pkg-config`, the hidapi development package, Node.js,
`jq`, and `bsdtar`, then run:

```sh
make check
make package
```

The installable archive is written to `dist/`.

## OpenDeck layout compatibility

The plugin requests `encoder_position: "top"` when registering the device.
OpenDeck versions without that optional layout property still work, but render
the encoder row below the keys. A companion OpenDeck change adds the generic
property without special-casing the Galleon.

## License

GPL-3.0-or-later. See [`LICENSE.md`](LICENSE.md).

## Development disclosure

This plugin was developed with substantial AI assistance and then reviewed and
tested against physical Galleon 100 SD hardware. No proprietary binaries,
firmware, extracted assets, or reverse-engineering work files are included in
this repository.
