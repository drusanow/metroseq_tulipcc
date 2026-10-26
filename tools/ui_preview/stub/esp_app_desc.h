#pragma once
typedef struct {
    char project_name[32];
} esp_app_desc_t;
static inline const esp_app_desc_t *esp_app_get_description(void) {
    static const esp_app_desc_t d = {"studio"};
    return &d;
}
