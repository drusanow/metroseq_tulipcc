// lv_mem_psram.c - LVGL's allocator: widgets, styles and texts live in the
// 8 MB PSRAM (as on the original Tulip), not in the ~300 KB of internal
// RAM that DMA buffers, task stacks and the real-time code need. A Studio
// image builds several apps' screens; with the C library's malloc (which
// keeps every block under 4 KB internal) they would eat internal RAM.
// The panel's render band is allocated separately, in internal RAM.
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include "esp_heap_caps.h"

#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) {
    void *p = heap_caps_malloc(size, CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);   // PSRAM full: anywhere
}

void *lv_realloc_core(void *p, size_t new_size) {
    void *q = heap_caps_realloc(p, new_size, CAPS);
    return q ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, CAPS);
    mon->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon->free_size = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->used_cnt = info.allocated_blocks;
    mon->free_cnt = info.free_blocks;
    mon->max_used = info.total_allocated_bytes;
    mon->used_pct = mon->total_size ? (uint8_t)(100 * info.total_allocated_bytes / mon->total_size) : 0;
    mon->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif
