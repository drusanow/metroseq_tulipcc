// esp_partition.h for the preview: the "samples" partition is the packed
// image the tests build (tests/build/gamma9001.bin).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef int esp_partition_subtype_t;
typedef struct {
    uint32_t address, size;
    const char *path;
} esp_partition_t;
const esp_partition_t *esp_partition_find_first(esp_partition_type_t t, esp_partition_subtype_t s,
                                                const char *label);
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n);
