# Host build of the hardware-independent engine + its tests.
#   make -C tests          build
#   make -C tests test     unit tests + parity against the Python original
ENGINE = ../modules/mod_metroseq/engine
STUDIO = ../components/tulip_studio
GX = ../modules/mod_gintonix/core
# the cJSON ESP-IDF ships (MIT), vendored so the host tests need no IDF
CJSON = vendor/cjson
GX_SRC = $(GX)/gx_seq.c $(GX)/gx_model.c $(GX)/gx_factory.c $(GX)/gx_json.c $(GX)/gx_dsp.c \
         $(GX)/gx_engine.c $(CJSON)/cJSON.c $(STUDIO)/src/tclock.c
HAL = ../components/tulip_hal
LCORE = ../launcher/main/core
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined
CFLAGS += -I$(ENGINE) -I$(HAL)/include -I$(LCORE) -I$(STUDIO)/include
SRC = $(ENGINE)/seq_engine.c $(ENGINE)/seq_model.c $(ENGINE)/seq_clock.c $(HAL)/src/touch_cal.c \
      $(STUDIO)/src/tclock.c $(STUDIO)/src/midi_parse.c
HDR = $(wildcard $(ENGINE)/*.h)

all: build/test_engine build/test_midi_touch build/test_launcher build/test_studio build/test_gx build/gx_tool build/test_sloe build/test_wavetron build/sim

build/test_engine: test_engine.c $(SRC) $(HDR) $(ENGINE)/seq_json.c
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(CJSON) -o $@ test_engine.c $(SRC) $(ENGINE)/seq_json.c $(CJSON)/cJSON.c -lm

build/test_midi_touch: test_midi_touch.c $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_midi_touch.c $(SRC) -lm

build/test_launcher: test_launcher.c $(LCORE)/path.c $(LCORE)/appimg.c $(LCORE)/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_launcher.c $(LCORE)/path.c $(LCORE)/appimg.c

build/test_studio: test_studio.c $(STUDIO)/src/tclock.c $(STUDIO)/src/midi_parse.c $(STUDIO)/include/studio/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_studio.c $(STUDIO)/src/tclock.c $(STUDIO)/src/midi_parse.c

build/test_gx: test_gx.c $(GX_SRC) $(wildcard $(GX)/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(GX) -I$(CJSON) -o $@ test_gx.c $(GX_SRC) -lm

build/gx_tool: gx_tool.c $(GX_SRC) $(wildcard $(GX)/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(GX) -I$(CJSON) -o $@ gx_tool.c $(GX_SRC) -lm

build/test_sloe: test_sloe.c ../modules/mod_sloe/sloe_dsp.c ../modules/mod_sloe/sloe_dsp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I../modules/mod_sloe -o $@ test_sloe.c ../modules/mod_sloe/sloe_dsp.c -lm

WT = ../modules/mod_wavetron/core
WT_SRC = $(WT)/wt_params.c $(WT)/wt_tables.c $(WT)/wt_dsp.c $(WT)/wt_fx.c $(WT)/wt_patch.c $(CJSON)/cJSON.c
build/test_wavetron: test_wavetron.c $(WT_SRC) $(wildcard $(WT)/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(WT) -I$(CJSON) -o $@ test_wavetron.c $(WT_SRC) -lm

build/sim: sim.c $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ sim.c $(SRC) -lm

test: all
	./build/test_engine
	./build/test_midi_touch
	./build/test_launcher
	./build/test_studio
	./build/test_gx
	./build/test_sloe
	./build/test_wavetron
	python3 gx_parity.py
	python3 py_parity.py

clean:
	rm -rf build

.PHONY: all test clean
