// web.c - see web.h.
#include "web.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "apps.h"
#include "core/path.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fsutil.h"
#include "lsettings.h"
#include "tulip/fs.h"
#include "wifi.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_srv;

#define CHUNK 4096
#define MAX_FAILS 5
#define LOCKOUT_US (30LL * 1000000)

static int s_fails;
static int64_t s_locked_until;

// --- helpers -----------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *r, int status, const char *body) {
    char st[48];
    snprintf(st, sizeof st, "%d %s", status,
             status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized"
                              : status == 404 ? "Not Found" : status == 409 ? "Conflict"
                              : status == 429 ? "Too Many Requests" : status == 507 ? "Insufficient Storage"
                              : "Internal Server Error");
    httpd_resp_set_status(r, st);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, body);
}

static esp_err_t err_json(httpd_req_t *r, int status, const char *msg) {
    char b[160];
    snprintf(b, sizeof b, "{\"ok\":false,\"error\":\"%s\"}", msg);
    return send_json(r, status, b);
}

// Escape a string into a JSON string literal (without quotes).
static void json_esc(char *dst, size_t n, const char *src) {
    size_t w = 0;
    for (; *src && w + 7 < n; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[w++] = '\\';
            dst[w++] = (char)c;
        } else if (c < 0x20) {
            w += snprintf(dst + w, n - w, "\\u%04x", c);
        } else {
            dst[w++] = (char)c;
        }
    }
    dst[w] = 0;
}

static bool authorised(httpd_req_t *r) {
    int64_t now = esp_timer_get_time();
    if (now < s_locked_until) {
        err_json(r, 429, "too many wrong PINs - wait 30 s");
        return false;
    }
    char pin[8] = {0};
    if (httpd_req_get_hdr_value_str(r, "X-Tulip-Pin", pin, sizeof pin) == ESP_OK &&
        strcmp(pin, g_ls.pin) == 0) {
        s_fails = 0;
        return true;
    }
    if (++s_fails >= MAX_FAILS) {
        s_fails = 0;
        s_locked_until = now + LOCKOUT_US;
    }
    err_json(r, 401, "wrong or missing PIN");
    return false;
}

// Query parameter -> absolute, safe path under /files.
static bool get_path(httpd_req_t *r, const char *key, char *out, size_t n) {
    char q[512], v[300];
    if (httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) return false;
    if (!url_decode(v)) return false;
    return path_resolve(TULIP_FS_ROOT, v, out, n);
}

static const char *rel(const char *abs) {
    const char *p = abs + strlen(TULIP_FS_ROOT);
    return *p ? p : "/";
}

// --- handlers ------------------------------------------------------------------

static esp_err_t h_index(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_info(httpd_req_t *r) {
    if (!authorised(r)) return ESP_OK;
    uint64_t total = 0, fr = 0;
    tulip_fs_usage(&total, &fr);
    static app_entry_t apps[MAX_APPS];
    int n = apps_scan(apps, MAX_APPS);
    httpd_resp_set_type(r, "application/json");
    char b[600], e1[200], e2[80], e3[80];
    snprintf(b, sizeof b,
             "{\"ok\":true,\"device\":\"Tulip CC\",\"launcher\":\"%s\",\"ip\":\"%s\","
             "\"total\":%llu,\"free\":%llu,\"apps\":[",
             esp_app_get_description()->version, g_wifi.ip, total, fr);
    httpd_resp_sendstr_chunk(r, b);
    for (int i = 0; i < n; i++) {
        json_esc(e1, sizeof e1, rel(apps[i].path));
        json_esc(e2, sizeof e2, apps[i].info.name);
        json_esc(e3, sizeof e3, apps[i].info.version);
        snprintf(b, sizeof b, "%s{\"path\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"size\":%lu,\"ready\":%s}",
                 i ? "," : "", e1, e2, e3, (unsigned long)apps[i].size,
                 apps[i].slot >= 0 ? "true" : "false");
        httpd_resp_sendstr_chunk(r, b);
    }
    httpd_resp_sendstr_chunk(r, "]}");
    return httpd_resp_sendstr_chunk(r, NULL);
}

static esp_err_t h_ls(httpd_req_t *r) {
    if (!authorised(r)) return ESP_OK;
    char path[300];
    if (!get_path(r, "path", path, sizeof path)) return err_json(r, 400, "bad path");
    static fs_entry_t ents[128];
    int n = fs_list(path, ents, 128);
    if (n < 0) return err_json(r, 404, "no such folder");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    char b[300], e[160];
    json_esc(e, sizeof e, rel(path));
    snprintf(b, sizeof b, "{\"ok\":true,\"path\":\"%s\",\"entries\":[", e);
    httpd_resp_sendstr_chunk(r, b);
    for (int i = 0; i < n; i++) {
        json_esc(e, sizeof e, ents[i].name);
        snprintf(b, sizeof b, "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%lu}", i ? "," : "", e,
                 ents[i].dir ? "true" : "false", (unsigned long)ents[i].size);
        httpd_resp_sendstr_chunk(r, b);
    }
    httpd_resp_sendstr_chunk(r, "]}");
    return httpd_resp_sendstr_chunk(r, NULL);
}

static esp_err_t h_get_file(httpd_req_t *r) {
    if (!authorised(r)) return ESP_OK;
    char path[300];
    if (!get_path(r, "path", path, sizeof path) || fs_is_dir(path))
        return err_json(r, 400, "bad path");
    FILE *f = fopen(path, "rb");
    if (!f) return err_json(r, 404, "no such file");
    char disp[120];
    snprintf(disp, sizeof disp, "attachment; filename=\"%s\"", path_basename(path));
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition", disp);
    static char buf[CHUNK];
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, sizeof buf, f)) > 0)
        err = httpd_resp_send_chunk(r, buf, n);
    fclose(f);
    if (err == ESP_OK) httpd_resp_send_chunk(r, NULL, 0);
    return err;
}

static esp_err_t h_put_file(httpd_req_t *r) {
    if (!authorised(r)) return ESP_OK;
    char path[300], parent[300], tmp[320];
    if (!get_path(r, "path", path, sizeof path) || !path_parent(path, parent, sizeof parent) ||
        !path_valid_name(path_basename(path)))
        return err_json(r, 400, "bad file name");
    if (!fs_is_dir(parent)) return err_json(r, 404, "folder does not exist");
    if (fs_is_dir(path)) return err_json(r, 409, "a folder has that name");
    uint64_t total = 0, fr = 0;
    tulip_fs_usage(&total, &fr);
    if (r->content_len + 64 * 1024 > fr) return err_json(r, 507, "not enough free space");

    // Write to a temporary file and rename at the end, so a broken upload
    // never leaves a half-written app behind.
    snprintf(tmp, sizeof tmp, "%s/.upload.tmp", parent);
    FILE *f = fopen(tmp, "wb");
    if (!f) return err_json(r, 500, "cannot create file");
    static char buf[CHUNK];
    size_t left = r->content_len;
    int timeouts = 0;
    while (left > 0) {
        int n = httpd_req_recv(r, buf, left < sizeof buf ? left : sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (n <= 0 || fwrite(buf, 1, n, f) != (size_t)n) {
            fclose(f);
            unlink(tmp);
            return err_json(r, 500, "upload interrupted");
        }
        timeouts = 0;
        left -= n;
    }
    fclose(f);
    if (!fs_rename(tmp, path)) {
        unlink(tmp);
        return err_json(r, 500, "could not store the file");
    }
    char b[300], e1[80], e2[80];
    app_info_t info;
    uint32_t size;
    if (path_has_ext(path, ".bin") && apps_read_info(path, &info, &size)) {
        json_esc(e1, sizeof e1, info.name);
        json_esc(e2, sizeof e2, info.version);
        snprintf(b, sizeof b, "{\"ok\":true,\"size\":%u,\"app\":{\"name\":\"%s\",\"version\":\"%s\"}}",
                 (unsigned)r->content_len, e1, e2);
    } else {
        snprintf(b, sizeof b, "{\"ok\":true,\"size\":%u}", (unsigned)r->content_len);
    }
    ESP_LOGI(TAG, "stored %s (%u bytes)", path, (unsigned)r->content_len);
    return send_json(r, 200, b);
}

static esp_err_t h_mkdir(httpd_req_t *r) {
    if (!authorised(r)) return ESP_OK;
    char path[300];
    if (!get_path(r, "path", path, sizeof path) || !path_valid_name(path_basename(path)))
        return err_json(r, 400, "bad folder name");
    if (fs_exists(path)) return err_json(r, 409, "already exists");
    return fs_mkdir(path) ? send_json(r, 200, "{\"ok\":true}") : err_json(r, 500, "mkdir failed");
}

static esp_err_t h_rm(httpd_req_t *r) {
    if (!authorised(r)) return ESP_OK;
    char path[300];
    if (!get_path(r, "path", path, sizeof path)) return err_json(r, 400, "bad path");
    if (strcmp(path, TULIP_FS_ROOT) == 0 || strcmp(path, TULIP_APPS_DIR) == 0)
        return err_json(r, 400, "that folder can't be deleted");
    if (!fs_exists(path)) return err_json(r, 404, "no such file");
    return fs_remove(path) ? send_json(r, 200, "{\"ok\":true}") : err_json(r, 500, "delete failed");
}

static esp_err_t h_mv(httpd_req_t *r) {
    if (!authorised(r)) return ESP_OK;
    char from[300], to[300];
    if (!get_path(r, "from", from, sizeof from) || !get_path(r, "to", to, sizeof to) ||
        !path_valid_name(path_basename(to)))
        return err_json(r, 400, "bad path");
    if (strcmp(from, TULIP_APPS_DIR) == 0) return err_json(r, 400, "the apps folder stays put");
    if (!fs_exists(from)) return err_json(r, 404, "no such file");
    if (fs_is_dir(to)) return err_json(r, 409, "a folder has that name");
    return fs_rename(from, to) ? send_json(r, 200, "{\"ok\":true}") : err_json(r, 500, "rename failed");
}

static esp_err_t h_launch(httpd_req_t *r) {
    if (!authorised(r)) return ESP_OK;
    char path[300];
    if (!get_path(r, "path", path, sizeof path)) return err_json(r, 400, "bad path");
    if (!apps_launch(path)) return err_json(r, 400, "not an app, or busy");
    return send_json(r, 200, "{\"ok\":true,\"message\":\"launching - the Tulip will restart\"}");
}

// --- server --------------------------------------------------------------------

bool web_running(void) { return s_srv != NULL; }

bool web_start(void) {
    if (s_srv) return true;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.core_id = 0;   // with Wi-Fi, away from the UI on core 1
    c.stack_size = 8192;
    c.max_uri_handlers = 16;
    c.lru_purge_enable = true;
    c.recv_wait_timeout = 10;
    c.send_wait_timeout = 10;
    if (httpd_start(&s_srv, &c) != ESP_OK) {
        s_srv = NULL;
        return false;
    }
    const httpd_uri_t uris[] = {
        {"/", HTTP_GET, h_index, NULL},
        {"/api/info", HTTP_GET, h_info, NULL},
        {"/api/ls", HTTP_GET, h_ls, NULL},
        {"/api/file", HTTP_GET, h_get_file, NULL},
        {"/api/file", HTTP_PUT, h_put_file, NULL},
        {"/api/mkdir", HTTP_POST, h_mkdir, NULL},
        {"/api/rm", HTTP_POST, h_rm, NULL},
        {"/api/mv", HTTP_POST, h_mv, NULL},
        {"/api/launch", HTTP_POST, h_launch, NULL},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(s_srv, &uris[i]);
    ESP_LOGI(TAG, "file transfer at http://tulip.local (PIN %s)", g_ls.pin);
    return true;
}

void web_stop(void) {
    if (!s_srv) return;
    httpd_stop(s_srv);
    s_srv = NULL;
}
