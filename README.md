# Wokwi GC9A01 custom chip

A Wokwi model of a 240x240 GC9A01 SPI display. It exposes `VCC`, `GND`, `CS`,
`DC`, `SCK`, `MOSI`, `BL`, and `RST`; the consuming project's `diagram.json`
assigns those signals to board pins.

The model handles hardware and software reset, sleep and display on/off state,
MADCTL axis mapping, RGB565 and RGB666 input, CASET/RASET windows, and RAMWR
cursor wrapping. Display RAM is retained while sleep, display-off, or the
backlight blanks the framebuffer. Pixel changes are sent to Wokwi in scanline
batches. `BL` duty is sampled every 16.667 ms and scales the displayed RGB
values without changing display RAM.

## Provenance

The model was developed from the custom-chip code in these public Wokwi
projects:

- https://wokwi.com/projects/444775402608685057
- https://wokwi.com/projects/462450676712637441

Neither referenced project states a license. This records provenance and does
not infer licensing terms.
