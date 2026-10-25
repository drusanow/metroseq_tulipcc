# Host build of the hardware-independent engine + its tests.
#   make -C tests          build
#   make -C tests test     unit tests + parity against the Python original
ENGINE = ../firmware/main/engine
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined
CFLAGS += -I$(ENGINE)
SRC = $(ENGINE)/seq_engine.c $(ENGINE)/seq_model.c
HDR = $(wildcard $(ENGINE)/*.h)

all: build/test_engine build/sim

build/test_engine: test_engine.c $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_engine.c $(SRC)

build/sim: sim.c $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ sim.c $(SRC)

test: all
	./build/test_engine
	python3 py_parity.py

clean:
	rm -rf build

.PHONY: all test clean
