# Galleon 100 SD protocol notes

USB identity:

- Vendor ID: `1b1c` (Corsair)
- Product ID: `2b18` (Galleon Stream Deck component)
- USB 2.0 high-speed composite HID device

Interface 0 (`hidraw`, control/display):

- Interrupt IN endpoint `0x81`, packet size 512 bytes
- Interrupt OUT endpoint `0x09`, packet size 512 bytes
- Input report ID `0x01`, payload 511 bytes
- Output report ID `0x02`, payload 1023 bytes
- Feature report IDs `0x03` through `0x15`, 31-byte payloads

Interfaces 1 and 2 expose standard keyboard and consumer-control reports. Interface 3 exposes Dynamic Lighting feature reports.

Physical layout:

- 12 keys: 3 columns × 4 rows, 160×160 pixels each
- Two encoders with four RGB ring segments each
- Touch/display strip: 720×384 pixels, exposed as two 360×384 OpenDeck encoder regions
- Composite internal panel: 720×1280 pixels

Observed while testing interoperability with Elgato Stream Deck 7.3.0:

- Keepalive/software mode on firmware `3.06.005`: feature report `03 27`, repeated every 500 ms.
- Firmware builds with patch number 6 or newer appear to use a different keepalive report beginning `02 25`; this branch is not implemented yet.
- Brightness: feature report `03 08 <value> 00 ...`.
- Solid key color: feature report `03 06 <key> <red> <green> <blue>`.
- Full-display transfers use report ID `02`, command `09`, 8-byte headers, and chunks of up to 1016 bytes.
- Other image commands observed are `07`, `08`, `0b`, and rectangular update `0c`.

The keepalive is implemented first because the Galleon stops reporting controls when it expires. Image payloads are JPEG; individual key images use command `07`, while LCD regions use command `0c`.

The implemented input decoder follows the Gen 2 framing used by `@elgato-stream-deck/core` 7.6.3:

- Input type `00`: key state bytes begin at offset 3.
- Input type `02`: touchscreen short/long presses and swipe coordinates.
- Input type `03`, subtype `00`: encoder press states.
- Input type `03`, subtype `01`: signed encoder rotation deltas.
