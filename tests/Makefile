# Host build of the hardware-independent engine + its tests.
#   make -C tests          build
#   make -C tests test     unit tests + parity against the Python original
ENGINE = ../firmware/main/engine
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined
CFLAGS += -I$(ENGINE)
SRC = $(ENGINE)/seq_engine.c $(ENGINE)/seq_model.c $(ENGINE)/midi_clock.c $(ENGINE)/touch_cal.c
HDR = $(wildcard $(ENGINE)/*.h)

all: build/test_engine build/test_midi_touch build/sim

build/test_engine: test_engine.c $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_engine.c $(SRC) -lm

build/test_midi_touch: test_midi_touch.c $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_midi_touch.c $(SRC) -lm

build/sim: sim.c $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ sim.c $(SRC) -lm

test: all
	./build/test_engine
	./build/test_midi_touch
	python3 py_parity.py

clean:
	rm -rf build

.PHONY: all test clean
