# Host render of the on-device UIs -> build/*.png (needs python3 + Pillow).
#   make -C tools/ui_preview
LVGL = ../../components/lvgl
HAL  = ../../components/tulip_hal
MS   = ../../apps/metroseq/main
LA   = ../../launcher/main
BASE = -O1 -g -w -std=gnu11 -DLV_CONF_INCLUDE_SIMPLE -Ibuild -Istub -I$(LVGL) -I$(HAL)/include
LV_SRC := $(shell find $(LVGL)/src -name '*.c')
HAL_UI = $(HAL)/src/touch_cal.c $(HAL)/src/touchcal_ui.c harness.c

MS_SRC = metroseq_preview.c $(wildcard $(MS)/ui/*.c) $(MS)/engine/seq_model.c $(HAL_UI)
LA_SRC = launcher_preview.c $(wildcard $(LA)/ui/*.c) $(wildcard $(LA)/core/*.c) $(HAL_UI)

all: build/metroseq_preview build/launcher_preview
	cd build && rm -f *.ppm && ./metroseq_preview && ./launcher_preview
	python3 -c "import glob; from PIL import Image; [Image.open(p).save(p[:-4] + '.png') for p in glob.glob('build/*.ppm')]"
	rm -f build/*.ppm

build/lv_conf.h: gen_lv_conf.py
	python3 gen_lv_conf.py build

build/liblvgl.a: build/lv_conf.h
	mkdir -p build/obj
	@cd build/obj && for f in $(abspath $(LV_SRC)); do \
	  echo "$$f"; done | xargs -P8 -I{} sh -c 'cc $(BASE:-I%=-I$(abspath .)/%) -c {} -o $$(echo {} | md5sum | cut -c1-16).o'
	ar rcs $@ build/obj/*.o

build/metroseq_preview: build/liblvgl.a $(MS_SRC) harness.h
	cc $(BASE) -I$(MS) -o $@ $(MS_SRC) build/liblvgl.a -lm

build/launcher_preview: build/liblvgl.a $(LA_SRC) harness.h
	cc $(BASE) -I$(LA) -I$(LA)/ui -I$(LA)/core -o $@ $(LA_SRC) build/liblvgl.a -lm

clean:
	rm -rf build

.PHONY: all clean
