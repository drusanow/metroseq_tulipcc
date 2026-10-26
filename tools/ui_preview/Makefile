# Host render of the on-device UIs -> build/*.png (needs python3 + Pillow).
#   make -C tools/ui_preview
LVGL = ../../components/lvgl
HAL  = ../../components/tulip_hal
MS   = ../../modules/mod_metroseq
GXM  = ../../modules/mod_gintonix
SLM  = ../../modules/mod_sloe
ST   = ../../components/tulip_studio
CJ   = ../../tests/vendor/cjson
LA   = ../../launcher/main
BASE = -O1 -g -w -std=gnu11 -DLV_CONF_INCLUDE_SIMPLE -Ibuild -Istub -I$(LVGL) -I$(HAL)/include
LV_SRC := $(shell find $(LVGL)/src -name '*.c')
HAL_UI = $(HAL)/src/touch_cal.c $(HAL)/src/touchcal_ui.c harness.c

ST_SRC = studio_preview.c studio_stubs.c $(ST)/src/shell.c $(ST)/src/widgets.c $(ST)/src/studio.c \
         $(ST)/src/tclock.c $(wildcard $(MS)/ui/*.c) $(MS)/engine/seq_model.c \
         $(wildcard $(GXM)/core/*.c) $(GXM)/gx_mod.c $(wildcard $(GXM)/ui/*.c) \
         $(SLM)/sloe_dsp.c $(SLM)/sloe_mod.c $(CJ)/cJSON.c $(HAL_UI)
ST_INC = -I$(MS) -I$(MS)/platform -I$(GXM) -I$(GXM)/core -I$(GXM)/ui -I$(SLM) -I$(ST)/include \
         -I$(ST)/src -I$(CJ) -DTULIP_FS_ROOT=\"files\"
LA_SRC = launcher_preview.c $(wildcard $(LA)/ui/*.c) $(wildcard $(LA)/core/*.c) $(HAL_UI)

all: build/studio_preview build/launcher_preview
	cd build && rm -f *.ppm && rm -rf files && ./studio_preview && ./launcher_preview
	python3 -c "import glob; from PIL import Image; [Image.open(p).save(p[:-4] + '.png') for p in glob.glob('build/*.ppm')]"
	rm -f build/*.ppm

build/lv_conf.h: gen_lv_conf.py
	python3 gen_lv_conf.py build

build/liblvgl.a: build/lv_conf.h
	mkdir -p build/obj
	@cd build/obj && for f in $(abspath $(LV_SRC)); do \
	  echo "$$f"; done | xargs -P8 -I{} sh -c 'cc $(BASE:-I%=-I$(abspath .)/%) -c {} -o $$(echo {} | md5sum | cut -c1-16).o'
	ar rcs $@ build/obj/*.o

build/studio_preview: build/liblvgl.a $(ST_SRC) harness.h
	cc $(BASE) $(ST_INC) -o $@ $(ST_SRC) build/liblvgl.a -lm

build/launcher_preview: build/liblvgl.a $(LA_SRC) harness.h
	cc $(BASE) -I$(LA) -I$(LA)/ui -I$(LA)/core -o $@ $(LA_SRC) build/liblvgl.a -lm

clean:
	rm -rf build

.PHONY: all clean
