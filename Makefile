CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wshadow -Werror
BUILD := build
TEST := $(BUILD)/test_gc9a01

$(TEST): test/test_gc9a01.c test/wokwi-api.h gc9a01.chip.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest -o $@ $<

$(BUILD):
	mkdir -p $@

check: $(TEST)
	./$(TEST)

clean:
	rm -rf $(BUILD)

.PHONY: check clean

