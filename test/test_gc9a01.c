#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GC9A01_HOST_TEST 1
#include "../gc9a01.chip.c"

static uint8_t fake_framebuffer[PIXEL_COUNT * 4];
static uint32_t fake_pins[16];
static timer_config_t fake_timer;
static uint64_t fake_nanos;

pin_t pin_init(const char *name, uint32_t mode) {
  (void)name;
  (void)mode;
  return 1;
}

uint32_t pin_read(pin_t pin) {
  return fake_pins[pin];
}

bool pin_watch(pin_t pin, const pin_watch_config_t *config) {
  (void)pin;
  (void)config;
  return true;
}

spi_dev_t spi_init(const spi_config_t *config) {
  (void)config;
  return 1;
}

void spi_start(spi_dev_t spi, uint8_t *buffer, uint32_t count) {
  (void)spi;
  (void)buffer;
  (void)count;
}

void spi_stop(spi_dev_t spi) {
  (void)spi;
}

timer_t timer_init(const timer_config_t *config) {
  fake_timer = *config;
  return 1;
}

void timer_start(timer_t timer, uint32_t micros, bool repeat) {
  (void)timer;
  (void)micros;
  (void)repeat;
}

void timer_stop(timer_t timer) {
  (void)timer;
}

uint64_t get_sim_nanos(void) {
  return fake_nanos;
}

buffer_t framebuffer_init(uint32_t *width, uint32_t *height) {
  assert(*width == PANEL_W);
  assert(*height == PANEL_H);
  return 1;
}

void buffer_write(buffer_t buffer, uint32_t offset, void *data, uint32_t len) {
  assert(buffer == 1);
  assert(offset + len <= sizeof(fake_framebuffer));
  memcpy(fake_framebuffer + offset, data, len);
}

static chip_state_t state;

static const uint8_t *frame_pixel(uint16_t x, uint16_t y) {
  return &fake_framebuffer[((uint32_t)y * PANEL_W + x) * 4];
}

static const uint8_t *gram_pixel(uint16_t x, uint16_t y) {
  return &state.gram[((uint32_t)y * PANEL_W + x) * 4];
}

static void expect_rgb(const uint8_t *pixel, uint8_t r, uint8_t g, uint8_t b) {
  assert(pixel[0] == r);
  assert(pixel[1] == g);
  assert(pixel[2] == b);
  assert(pixel[3] == 0xff);
}

static void command(uint8_t code, const uint8_t *args, uint32_t count) {
  process_commands(&state, &code, 1);
  if (count != 0) {
    process_data(&state, args, count);
  }
}

static void set_window(uint16_t x, uint16_t y) {
  uint8_t column[4] = {
    (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)x,
  };
  uint8_t row[4] = {
    (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y >> 8), (uint8_t)y,
  };
  command(CMD_CASET, column, sizeof(column));
  command(CMD_RASET, row, sizeof(row));
}

static void setup(void) {
  memset(&state, 0, sizeof(state));
  memset(fake_framebuffer, 0xa5, sizeof(fake_framebuffer));
  memset(fake_pins, 0, sizeof(fake_pins));
  state.framebuffer = 1;
  state.bl_pin = 3;
  state.bl_sample_timer = 1;
  state.backlight_on = true;
  state.backlight_level = 255;
  fake_pins[state.bl_pin] = HIGH;
  controller_reset(&state, true);
  expect_rgb(frame_pixel(0, 0), 0, 0, 0);
  assert(state.sleep_mode);
  assert(!state.display_on);
}

static void show_display(void) {
  command(CMD_SLPOUT, NULL, 0);
  command(CMD_DISPON, NULL, 0);
  assert(output_visible(&state));
}

static void select_rgb565(void) {
  const uint8_t mode = 0x55;
  command(CMD_COLMOD, &mode, 1);
}

static void test_power_state_retains_gram(void) {
  setup();
  show_display();
  select_rgb565();
  set_window(0, 0);
  command(CMD_RAMWR, NULL, 0);
  const uint8_t red[2] = {0xf8, 0x00};
  process_data(&state, red, sizeof(red));
  expect_rgb(frame_pixel(0, 0), 255, 0, 0);

  command(CMD_DISPOFF, NULL, 0);
  expect_rgb(frame_pixel(0, 0), 0, 0, 0);
  expect_rgb(gram_pixel(0, 0), 255, 0, 0);
  command(CMD_DISPON, NULL, 0);
  expect_rgb(frame_pixel(0, 0), 255, 0, 0);

  command(CMD_SLPIN, NULL, 0);
  expect_rgb(frame_pixel(0, 0), 0, 0, 0);
  command(CMD_SLPOUT, NULL, 0);
  expect_rgb(frame_pixel(0, 0), 255, 0, 0);
}

static void test_invert_orientation_and_color_order(void) {
  setup();
  show_display();
  select_rgb565();
  const uint8_t madctl = MADCTL_MX | MADCTL_BGR;
  command(CMD_MADCTL, &madctl, 1);
  set_window(0, 0);
  command(CMD_RAMWR, NULL, 0);
  const uint8_t red[2] = {0xf8, 0x00};
  process_data(&state, red, sizeof(red));
  expect_rgb(frame_pixel(PANEL_W - 1, 0), 255, 0, 0);

  command(CMD_INVON, NULL, 0);
  assert(state.inverted);
  expect_rgb(frame_pixel(PANEL_W - 1, 0), 255, 0, 0);
  command(CMD_INVOFF, NULL, 0);
  assert(!state.inverted);
  expect_rgb(frame_pixel(PANEL_W - 1, 0), 255, 0, 0);
}

static void test_rgb666_survives_segment_split(void) {
  setup();
  show_display();
  const uint8_t mode = 0x66;
  command(CMD_COLMOD, &mode, 1);
  set_window(2, 3);
  command(CMD_RAMWR, NULL, 0);
  const uint8_t first[2] = {0xfc, 0x00};
  const uint8_t last = 0x00;
  process_data(&state, first, sizeof(first));
  expect_rgb(frame_pixel(2, 3), 0, 0, 0);
  process_data(&state, &last, 1);
  expect_rgb(frame_pixel(2, 3), 255, 0, 0);
}

static void test_backlight_and_reset(void) {
  setup();
  show_display();
  select_rgb565();
  set_window(0, 0);
  command(CMD_RAMWR, NULL, 0);
  const uint8_t green[2] = {0x07, 0xe0};
  process_data(&state, green, sizeof(green));
  expect_rgb(frame_pixel(0, 0), 0, 255, 0);

  fake_nanos = 0;
  state.bl_sample_start_ns = 0;
  state.bl_last_edge_ns = 0;
  fake_nanos = 4000000;
  on_backlight_change(&state, state.bl_pin, LOW);
  fake_pins[state.bl_pin] = LOW;
  fake_nanos = 16000000;
  on_backlight_sample(&state);
  expect_rgb(frame_pixel(0, 0), 0, 64, 0);

  fake_nanos = 32000000;
  on_backlight_sample(&state);
  expect_rgb(frame_pixel(0, 0), 0, 0, 0);
  fake_pins[state.bl_pin] = HIGH;
  on_backlight_change(&state, state.bl_pin, HIGH);
  fake_nanos = 48000000;
  on_backlight_sample(&state);
  expect_rgb(frame_pixel(0, 0), 0, 255, 0);

  command(CMD_SWRESET, NULL, 0);
  expect_rgb(gram_pixel(0, 0), 0, 0, 0);
  expect_rgb(frame_pixel(0, 0), 0, 0, 0);
  assert(state.sleep_mode);
  assert(!state.display_on);
}

int main(void) {
  test_power_state_retains_gram();
  test_invert_orientation_and_color_order();
  test_rgb666_survives_segment_split();
  test_backlight_and_reset();
  puts("gc9a01 custom-chip tests: PASS");
  return 0;
}
