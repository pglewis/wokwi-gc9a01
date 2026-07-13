// Reusable Wokwi custom-chip model of the GC9A01 240x240 SPI TFT.
//
// Board pin assignments belong in the consuming diagram. This model keeps the
// reference chip's controller behavior and adds retained display RAM, scanline-
// batched framebuffer updates, and PWM backlight simulation.
// Provenance and reference-project links are recorded in README.md beside this file.

#ifdef GC9A01_HOST_TEST
#include "test/wokwi-api.h"
#else
#include "wokwi-api.h"
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_W 240u
#define PANEL_H 240u
#define PIXEL_COUNT (PANEL_W * PANEL_H)

// GC9A01 / MIPI DCS commands modeled here.
#define CMD_SWRESET  0x01
#define CMD_SLPIN    0x10
#define CMD_SLPOUT   0x11
#define CMD_PTLON    0x12
#define CMD_NORON    0x13
#define CMD_INVOFF   0x20
#define CMD_INVON    0x21
#define CMD_DISPOFF  0x28
#define CMD_DISPON   0x29
#define CMD_CASET    0x2A
#define CMD_RASET    0x2B
#define CMD_RAMWR    0x2C
#define CMD_MADCTL   0x36
#define CMD_COLMOD   0x3A
#define CMD_FRAMERATE 0xE8
#define CMD_GAMMA1   0xF0
#define CMD_GAMMA2   0xF1
#define CMD_GAMMA3   0xF2
#define CMD_GAMMA4   0xF3

#define MADCTL_BGR 0x08
#define MADCTL_MV  0x20
#define MADCTL_MX  0x40
#define MADCTL_MY  0x80

#define BL_SAMPLE_US 16667u

typedef struct {
  pin_t cs_pin;
  pin_t dc_pin;
  pin_t rst_pin;
  pin_t bl_pin;
  spi_dev_t spi;
  buffer_t framebuffer;
  timer_t bl_sample_timer;

  uint8_t spi_rx[1024];
  uint8_t gram[PIXEL_COUNT * 4];
  uint8_t scanline[PANEL_W * 4];

  uint8_t command;
  uint8_t command_args[16];
  uint8_t command_arg_count;
  uint8_t command_arg_need;
  uint8_t pixel_bytes[3];
  uint8_t pixel_byte_count;

  uint16_t x0;
  uint16_t x1;
  uint16_t y0;
  uint16_t y1;
  uint16_t cx;
  uint16_t cy;

  uint8_t madctl;
  uint8_t color_mode;
  bool ram_write;
  bool display_on;
  bool sleep_mode;
  bool inverted;
  bool backlight_on;
  uint8_t backlight_level;
  uint64_t bl_sample_start_ns;
  uint64_t bl_last_edge_ns;
  uint64_t bl_high_ns;
  bool capturing;
  int segment_dc;
} chip_state_t;

typedef struct {
  uint16_t x0;
  uint16_t x1;
  uint16_t y0;
  uint16_t y1;
  bool valid;
} dirty_rect_t;

static bool output_visible(const chip_state_t *s) {
  return s->display_on && !s->sleep_mode && s->backlight_on;
}

static void fill_black(chip_state_t *s) {
  memset(s->scanline, 0, sizeof(s->scanline));
  for (uint32_t x = 0; x < PANEL_W; x++) {
    s->scanline[x * 4 + 3] = 0xff;
  }
  for (uint32_t y = 0; y < PANEL_H; y++) {
    buffer_write(s->framebuffer, y * PANEL_W * 4, s->scanline,
      sizeof(s->scanline));
  }
}

static void present_rect(chip_state_t *s, uint16_t x0, uint16_t x1,
    uint16_t y0, uint16_t y1) {
  if (!output_visible(s) || x0 >= PANEL_W || y0 >= PANEL_H) {
    return;
  }
  if (x1 >= PANEL_W) {
    x1 = PANEL_W - 1;
  }
  if (y1 >= PANEL_H) {
    y1 = PANEL_H - 1;
  }
  uint32_t width = (uint32_t)x1 - x0 + 1;
  for (uint32_t y = y0; y <= y1; y++) {
    const uint8_t *src = &s->gram[(y * PANEL_W + x0) * 4];
    if (s->backlight_level != 255) {
      for (uint32_t x = 0; x < width; x++) {
        s->scanline[x * 4] = (uint8_t)(((uint32_t)src[x * 4] * s->backlight_level) / 255u);
        s->scanline[x * 4 + 1] = (uint8_t)(((uint32_t)src[x * 4 + 1] * s->backlight_level) / 255u);
        s->scanline[x * 4 + 2] = (uint8_t)(((uint32_t)src[x * 4 + 2] * s->backlight_level) / 255u);
        s->scanline[x * 4 + 3] = 0xff;
      }
      src = s->scanline;
    }
    // Wokwi's API predates const-correctness; buffer_write does not modify src.
    buffer_write(s->framebuffer, (y * PANEL_W + x0) * 4, (void *)src, width * 4);
  }
}

static void present_all(chip_state_t *s) {
  present_rect(s, 0, PANEL_W - 1, 0, PANEL_H - 1);
}

static void refresh_output(chip_state_t *s) {
  if (output_visible(s)) {
    present_all(s);
  } else {
    fill_black(s);
  }
}

static void clear_gram(chip_state_t *s) {
  memset(s->gram, 0, sizeof(s->gram));
  for (uint32_t i = 0; i < PIXEL_COUNT; i++) {
    s->gram[i * 4 + 3] = 0xff;
  }
}

static void controller_reset(chip_state_t *s, bool clear_pixels) {
  s->command = 0;
  s->command_arg_count = 0;
  s->command_arg_need = 0;
  s->pixel_byte_count = 0;
  s->x0 = 0;
  s->x1 = PANEL_W - 1;
  s->y0 = 0;
  s->y1 = PANEL_H - 1;
  s->cx = 0;
  s->cy = 0;
  s->madctl = 0;
  s->color_mode = 0x06; // GC9A01 reset default: 18 bits per pixel
  s->ram_write = false;
  s->display_on = false;
  s->sleep_mode = true;
  s->inverted = false;
  if (clear_pixels) {
    clear_gram(s);
  }
  fill_black(s);
}

static uint8_t command_arg_size(uint8_t command) {
  switch (command) {
  case CMD_MADCTL:
  case CMD_COLMOD:
  case CMD_FRAMERATE:
    return 1;
  case CMD_CASET:
  case CMD_RASET:
    return 4;
  case CMD_GAMMA1:
  case CMD_GAMMA2:
  case CMD_GAMMA3:
  case CMD_GAMMA4:
    return 6;
  default:
    return 0;
  }
}

static void execute_command(chip_state_t *s) {
  switch (s->command) {
  case CMD_SWRESET:
    controller_reset(s, true);
    return;
  case CMD_SLPIN:
    s->sleep_mode = true;
    refresh_output(s);
    break;
  case CMD_SLPOUT:
    s->sleep_mode = false;
    refresh_output(s);
    break;
  case CMD_PTLON:
  case CMD_NORON:
    break;
  case CMD_INVOFF:
    s->inverted = false;
    break;
  case CMD_INVON:
    s->inverted = true;
    break;
  case CMD_DISPOFF:
    s->display_on = false;
    refresh_output(s);
    break;
  case CMD_DISPON:
    s->display_on = true;
    refresh_output(s);
    break;
  case CMD_CASET:
    s->x0 = (uint16_t)((s->command_args[0] << 8) | s->command_args[1]);
    s->x1 = (uint16_t)((s->command_args[2] << 8) | s->command_args[3]);
    s->cx = s->x0;
    break;
  case CMD_RASET:
    s->y0 = (uint16_t)((s->command_args[0] << 8) | s->command_args[1]);
    s->y1 = (uint16_t)((s->command_args[2] << 8) | s->command_args[3]);
    s->cy = s->y0;
    break;
  case CMD_RAMWR:
    s->ram_write = true;
    s->pixel_byte_count = 0;
    s->cx = s->x0;
    s->cy = s->y0;
    break;
  case CMD_MADCTL:
    s->madctl = s->command_args[0];
    break;
  case CMD_COLMOD:
    s->color_mode = s->command_args[0];
    s->pixel_byte_count = 0;
    break;
  default:
    break;
  }
}

static void process_commands(chip_state_t *s, const uint8_t *buf, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    s->command = buf[i];
    s->command_arg_count = 0;
    s->command_arg_need = command_arg_size(s->command);
    s->ram_write = false;
    s->pixel_byte_count = 0;
    if (s->command_arg_need == 0) {
      execute_command(s);
    }
  }
}

static void process_command_args(chip_state_t *s, const uint8_t *buf, uint32_t n) {
  for (uint32_t i = 0; i < n && s->command_arg_count < s->command_arg_need; i++) {
    s->command_args[s->command_arg_count++] = buf[i];
    if (s->command_arg_count == s->command_arg_need) {
      execute_command(s);
    }
  }
}

static void dirty_add(dirty_rect_t *d, uint16_t x, uint16_t y) {
  if (!d->valid) {
    d->x0 = d->x1 = x;
    d->y0 = d->y1 = y;
    d->valid = true;
    return;
  }
  if (x < d->x0) d->x0 = x;
  if (x > d->x1) d->x1 = x;
  if (y < d->y0) d->y0 = y;
  if (y > d->y1) d->y1 = y;
}

static bool map_cursor(const chip_state_t *s, uint16_t *out_x, uint16_t *out_y) {
  uint32_t x = s->cx;
  uint32_t y = s->cy;
  if (s->madctl & MADCTL_MV) {
    uint32_t t = x;
    x = y;
    y = t;
  }
  if (s->madctl & MADCTL_MX) {
    x = x < PANEL_W ? PANEL_W - 1 - x : PANEL_W;
  }
  if (s->madctl & MADCTL_MY) {
    y = y < PANEL_H ? PANEL_H - 1 - y : PANEL_H;
  }
  if (x >= PANEL_W || y >= PANEL_H) {
    return false;
  }
  *out_x = (uint16_t)x;
  *out_y = (uint16_t)y;
  return true;
}

static void advance_cursor(chip_state_t *s) {
  if (s->cx < s->x1) {
    s->cx++;
    return;
  }
  s->cx = s->x0;
  s->cy = s->cy < s->y1 ? (uint16_t)(s->cy + 1) : s->y0;
}

static void store_pixel(chip_state_t *s, uint8_t r, uint8_t g, uint8_t b,
    dirty_rect_t *dirty) {
  // MADCTL_BGR selects the controller-to-glass element order; the SPI payload is
  // still the caller's logical RGB565/RGB666 color. Swapping channels here makes
  // the LCDKit driver's red pixels blue and double-models a physical panel detail.
  uint16_t x;
  uint16_t y;
  if (map_cursor(s, &x, &y)) {
    uint8_t *dst = &s->gram[((uint32_t)y * PANEL_W + x) * 4];
    dst[0] = r;
    dst[1] = g;
    dst[2] = b;
    dst[3] = 0xff;
    dirty_add(dirty, x, y);
  }
  advance_cursor(s);
}

static void store_rgb565(chip_state_t *s, const uint8_t *p, dirty_rect_t *dirty) {
  uint16_t c = (uint16_t)((p[0] << 8) | p[1]);
  uint8_t r5 = (uint8_t)((c >> 11) & 0x1f);
  uint8_t g6 = (uint8_t)((c >> 5) & 0x3f);
  uint8_t b5 = (uint8_t)(c & 0x1f);
  store_pixel(s, (uint8_t)((r5 << 3) | (r5 >> 2)),
    (uint8_t)((g6 << 2) | (g6 >> 4)),
    (uint8_t)((b5 << 3) | (b5 >> 2)), dirty);
}

static void store_rgb666(chip_state_t *s, const uint8_t *p, dirty_rect_t *dirty) {
  uint8_t r6 = (uint8_t)(p[0] >> 2);
  uint8_t g6 = (uint8_t)(p[1] >> 2);
  uint8_t b6 = (uint8_t)(p[2] >> 2);
  store_pixel(s, (uint8_t)((r6 << 2) | (r6 >> 4)),
    (uint8_t)((g6 << 2) | (g6 >> 4)),
    (uint8_t)((b6 << 2) | (b6 >> 4)), dirty);
}

static void process_pixels(chip_state_t *s, const uint8_t *buf, uint32_t n) {
  dirty_rect_t dirty = {0};
  uint8_t bytes_per_pixel = (s->color_mode & 0x07) == 0x06 ? 3 : 2;
  for (uint32_t i = 0; i < n; i++) {
    s->pixel_bytes[s->pixel_byte_count++] = buf[i];
    if (s->pixel_byte_count != bytes_per_pixel) {
      continue;
    }
    if (bytes_per_pixel == 3) {
      store_rgb666(s, s->pixel_bytes, &dirty);
    } else {
      store_rgb565(s, s->pixel_bytes, &dirty);
    }
    s->pixel_byte_count = 0;
  }
  if (dirty.valid) {
    present_rect(s, dirty.x0, dirty.x1, dirty.y0, dirty.y1);
  }
}

static void process_data(chip_state_t *s, const uint8_t *buf, uint32_t n) {
  if (s->ram_write) {
    process_pixels(s, buf, n);
  } else if (s->command_arg_need != 0) {
    process_command_args(s, buf, n);
  }
}

static void on_spi_done(void *user_data, uint8_t *buffer, uint32_t count) {
  chip_state_t *s = (chip_state_t *)user_data;
  if (count != 0) {
    if (s->segment_dc == LOW) {
      process_commands(s, buffer, count);
    } else {
      process_data(s, buffer, count);
    }
  }
  if (s->capturing && pin_read(s->cs_pin) == LOW) {
    spi_start(s->spi, s->spi_rx, sizeof(s->spi_rx));
  }
}

static void on_cs_change(void *user_data, pin_t pin, uint32_t value) {
  chip_state_t *s = (chip_state_t *)user_data;
  (void)pin;
  if (value == LOW) {
    s->capturing = true;
    s->segment_dc = (int)pin_read(s->dc_pin);
    spi_start(s->spi, s->spi_rx, sizeof(s->spi_rx));
  } else {
    s->capturing = false;
    spi_stop(s->spi);
  }
}

static void on_dc_change(void *user_data, pin_t pin, uint32_t value) {
  chip_state_t *s = (chip_state_t *)user_data;
  (void)pin;
  if (s->capturing) {
    // spi_stop flushes the preceding bytes through on_spi_done using the old D/C
    // level; the callback owns any restart while CS remains asserted.
    spi_stop(s->spi);
    s->segment_dc = (int)value;
  }
}

static void on_reset_change(void *user_data, pin_t pin, uint32_t value) {
  chip_state_t *s = (chip_state_t *)user_data;
  (void)pin;
  if (value == LOW) {
    s->capturing = false;
    spi_stop(s->spi);
    controller_reset(s, true);
  }
}

static void on_backlight_change(void *user_data, pin_t pin, uint32_t value) {
  chip_state_t *s = (chip_state_t *)user_data;
  (void)pin;
  uint64_t now = get_sim_nanos();
  // The callback value is the new level, so a falling edge closes a high interval.
  if (value == LOW) {
    s->bl_high_ns += now - s->bl_last_edge_ns;
  }
  s->bl_last_edge_ns = now;
}

static void on_backlight_sample(void *user_data) {
  chip_state_t *s = (chip_state_t *)user_data;
  uint64_t now = get_sim_nanos();
  if (pin_read(s->bl_pin) == HIGH) {
    s->bl_high_ns += now - s->bl_last_edge_ns;
  }
  uint64_t elapsed = now - s->bl_sample_start_ns;
  uint8_t level = elapsed ? (uint8_t)((s->bl_high_ns * 255u + elapsed / 2u) / elapsed) : 0;
  s->bl_sample_start_ns = now;
  s->bl_last_edge_ns = now;
  s->bl_high_ns = 0;
  if (level != s->backlight_level) {
    s->backlight_level = level;
    s->backlight_on = level != 0;
    refresh_output(s);
  }
}

void chip_init(void) {
  chip_state_t *s = calloc(1, sizeof(*s));
  if (!s) {
    return;
  }

  s->cs_pin = pin_init("CS", INPUT_PULLUP);
  s->dc_pin = pin_init("DC", INPUT);
  s->rst_pin = pin_init("RST", INPUT_PULLUP);
  s->bl_pin = pin_init("BL", INPUT_PULLUP);
  pin_init("VCC", INPUT);
  pin_init("GND", INPUT);

  uint32_t width = PANEL_W;
  uint32_t height = PANEL_H;
  s->framebuffer = framebuffer_init(&width, &height);
  s->backlight_on = pin_read(s->bl_pin) != LOW;
  s->backlight_level = s->backlight_on ? 255 : 0;
  s->bl_sample_start_ns = get_sim_nanos();
  s->bl_last_edge_ns = s->bl_sample_start_ns;
  const timer_config_t bl_timer_config = {
    .callback = on_backlight_sample, .user_data = s,
  };
  s->bl_sample_timer = timer_init(&bl_timer_config);
  timer_start(s->bl_sample_timer, BL_SAMPLE_US, true);
  controller_reset(s, true);

  const spi_config_t spi_config = {
    .sck = pin_init("SCK", INPUT),
    .mosi = pin_init("MOSI", INPUT),
    .miso = NO_PIN,
    .mode = 0,
    .done = on_spi_done,
    .user_data = s,
  };
  s->spi = spi_init(&spi_config);

  const pin_watch_config_t cs_watch = {
    .edge = BOTH, .pin_change = on_cs_change, .user_data = s,
  };
  const pin_watch_config_t dc_watch = {
    .edge = BOTH, .pin_change = on_dc_change, .user_data = s,
  };
  const pin_watch_config_t rst_watch = {
    .edge = BOTH, .pin_change = on_reset_change, .user_data = s,
  };
  const pin_watch_config_t bl_watch = {
    .edge = BOTH, .pin_change = on_backlight_change, .user_data = s,
  };
  pin_watch(s->cs_pin, &cs_watch);
  pin_watch(s->dc_pin, &dc_watch);
  pin_watch(s->rst_pin, &rst_watch);
  pin_watch(s->bl_pin, &bl_watch);
}
