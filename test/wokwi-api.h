#ifndef TEST_WOKWI_API_H
#define TEST_WOKWI_API_H

#include <stdbool.h>
#include <stdint.h>

enum { LOW = 0, HIGH = 1 };
enum { INPUT = 0, INPUT_PULLUP = 2 };
enum { BOTH = 3 };

typedef int32_t pin_t;
typedef uint32_t spi_dev_t;
typedef uint32_t buffer_t;
typedef uint32_t timer_t;
#define NO_PIN ((pin_t)-1)

typedef struct {
  void *user_data;
  uint32_t edge;
  void (*pin_change)(void *user_data, pin_t pin, uint32_t value);
} pin_watch_config_t;

typedef struct {
  void *user_data;
  pin_t sck;
  pin_t mosi;
  pin_t miso;
  uint32_t mode;
  void (*done)(void *user_data, uint8_t *buffer, uint32_t count);
  uint32_t reserved[8];
} spi_config_t;

typedef struct {
  void *user_data;
  void (*callback)(void *user_data);
  uint32_t reserved[8];
} timer_config_t;

pin_t pin_init(const char *name, uint32_t mode);
uint32_t pin_read(pin_t pin);
bool pin_watch(pin_t pin, const pin_watch_config_t *config);
spi_dev_t spi_init(const spi_config_t *config);
void spi_start(spi_dev_t spi, uint8_t *buffer, uint32_t count);
void spi_stop(spi_dev_t spi);
timer_t timer_init(const timer_config_t *config);
void timer_start(timer_t timer, uint32_t micros, bool repeat);
void timer_stop(timer_t timer);
uint64_t get_sim_nanos(void);
buffer_t framebuffer_init(uint32_t *width, uint32_t *height);
void buffer_write(buffer_t buffer, uint32_t offset, void *data, uint32_t len);

#endif
