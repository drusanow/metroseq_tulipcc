"""Write build/lv_conf.h from LVGL's template with the same settings as
firmware/sdkconfig.defaults (RGB565, CLIB malloc, Montserrat 16, dark theme)."""
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
tmpl = os.path.join(here, "..", "..", "firmware", "components", "lvgl", "lv_conf_template.h")
src = open(tmpl).read()
src = src.replace('#if 0 /*Set it to "1" to enable content*/', "#if 1", 1)
SETTINGS = {
    "LV_COLOR_DEPTH": "16",
    "LV_USE_STDLIB_MALLOC": "LV_STDLIB_CLIB",
    "LV_USE_STDLIB_STRING": "LV_STDLIB_CLIB",
    "LV_USE_STDLIB_SPRINTF": "LV_STDLIB_CLIB",
    "LV_FONT_MONTSERRAT_16": "1",
    "LV_FONT_DEFAULT": "&lv_font_montserrat_16",
    "LV_THEME_DEFAULT_DARK": "1",
    "LV_BUILD_EXAMPLES": "0",
}
for name, val in SETTINGS.items():
    pat = re.compile(r"(#define\s+%s\s+)(\S+)" % re.escape(name))
    if not pat.search(src):
        sys.exit("lv_conf template has no " + name)
    src = pat.sub(lambda m: m.group(1) + val, src, count=1)
os.makedirs(sys.argv[1], exist_ok=True)
open(os.path.join(sys.argv[1], "lv_conf.h"), "w").write(src)
