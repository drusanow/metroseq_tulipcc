# Host render of the firmware UI -> build/*.png (needs python3 + Pillow).
#   make -C tools/ui_preview
FW = ../../firmware/main
LVGL = ../../firmware/components/lvgl
CFLAGS = -O1 -g -w -std=gnu11 -DLV_CONF_INCLUDE_SIMPLE -Ibuild -Istub -I$(FW) -I$(LVGL)
LV_SRC := $(shell find $(LVGL)/src -name '*.c')
APP_SRC = preview.c $(wildcard $(FW)/ui/*.c) $(FW)/engine/seq_model.c $(FW)/engine/touch_cal.c

all: build/preview
	cd build && rm -f *.ppm && ./preview
	python3 -c "import glob; from PIL import Image; [Image.open(p).save(p[:-4] + '.png') for p in glob.glob('build/*.ppm')]"
	rm -f build/*.ppm

build/lv_conf.h: gen_lv_conf.py
	python3 gen_lv_conf.py build

build/liblvgl.a: build/lv_conf.h
	mkdir -p build/obj
	@cd build/obj && for f in $(abspath $(LV_SRC)); do \
	  echo "$$f"; done | xargs -P8 -I{} sh -c 'cc $(CFLAGS:-I%=-I$(abspath .)/%) -c {} -o $$(echo {} | md5sum | cut -c1-16).o'
	ar rcs $@ build/obj/*.o

build/preview: build/liblvgl.a $(APP_SRC)
	cc $(CFLAGS) -o $@ $(APP_SRC) build/liblvgl.a -lm

clean:
	rm -rf build

.PHONY: all clean
