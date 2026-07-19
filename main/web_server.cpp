#include "web_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"

#include "config_store.h"
#include "net_setup.h"
#include "valves.h"

static const char *TAG = "web_server";

extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_end");
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t settings_html_start[] asm("_binary_settings_html_start");
extern const uint8_t settings_html_end[] asm("_binary_settings_html_end");
extern const uint8_t advanced_html_start[] asm("_binary_advanced_html_start");
extern const uint8_t advanced_html_end[] asm("_binary_advanced_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");

static esp_err_t send_asset(httpd_req_t *req, const char *type, const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, type);
    // EMBED_TXTFILES appends a trailing null byte -- don't send it as body content.
    httpd_resp_send(req, (const char *)start, (end - start) - 1);
    return ESP_OK;
}

// Small hand-rolled base64 encoder for HTTP Basic Auth -- avoids pulling in
// mbedtls just to encode a "user:pass" string.
static void base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_size)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 4 < out_size; i += 3) {
        uint32_t n = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) n |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) n |= in[i + 2];
        out[o++] = tbl[(n >> 18) & 0x3F];
        out[o++] = tbl[(n >> 12) & 0x3F];
        out[o++] = (i + 1 < in_len) ? tbl[(n >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < in_len) ? tbl[n & 0x3F] : '=';
    }
    out[o] = '\0';
}

// Web-password protection is plain HTTP Basic Auth with no TLS -- acceptable
// only because this is a LAN-only tool and HTTPS was explicitly out of scope.
static bool check_auth(httpd_req_t *req)
{
    const app_config_t &cfg = config_get();
    if (cfg.web_password[0] == '\0') {
        return true;
    }

    char userpass[96];
    snprintf(userpass, sizeof(userpass), "admin:%s", cfg.web_password);
    char expected_b64[136];
    base64_encode((const unsigned char *)userpass, strlen(userpass), expected_b64, sizeof(expected_b64));
    char expected[160];
    snprintf(expected, sizeof(expected), "Basic %s", expected_b64);

    char auth_header[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK &&
        strcmp(auth_header, expected) == 0) {
        return true;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"irrigation\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    if (httpd_query_key_value(body, key, out, out_len) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    url_decode(out);
    return true;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)buf_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static void reboot_after_response(void)
{
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

// ---- AP setup mode handlers ----

static esp_err_t h_setup_page(httpd_req_t *req)
{
    return send_asset(req, "text/html", setup_html_start, setup_html_end);
}

static esp_err_t h_api_scan(httpd_req_t *req)
{
    char ssids[16][33];
    int n = net_setup_scan(ssids, 16);

    char resp[16 * 34 + 1];
    int pos = 0;
    for (int i = 0; i < n; i++) {
        pos += snprintf(resp + pos, sizeof(resp) - pos, "%s\n", ssids[i]);
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, pos);
    return ESP_OK;
}

static esp_err_t h_save_wifi(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    app_config_t cfg = config_get();
    char field[64];

    form_get(body, "ssid", field, sizeof(field));
    strlcpy(cfg.wifi_ssid, field, sizeof(cfg.wifi_ssid));

    form_get(body, "pass", field, sizeof(field));
    strlcpy(cfg.wifi_pass, field, sizeof(cfg.wifi_pass));

    if (form_get(body, "device_name", field, sizeof(field)) && field[0] != '\0') {
        strlcpy(cfg.device_name, field, sizeof(cfg.device_name));
    }

    if (cfg.wifi_ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    config_save(cfg);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    reboot_after_response();
    return ESP_OK;
}

// Answers every unmatched path with a redirect to the setup page, which is
// what makes phones/laptops auto-pop the captive portal after joining the AP.
static esp_err_t h_captive_catchall(httpd_req_t *req, httpd_err_code_t err)
{
    char location[48];
    snprintf(location, sizeof(location), "http://%s/", net_setup_ap_ip());
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- Normal (STA-connected) mode handlers ----

static esp_err_t h_index_page(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    return send_asset(req, "text/html", index_html_start, index_html_end);
}

static esp_err_t h_settings_page(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    return send_asset(req, "text/html", settings_html_start, settings_html_end);
}

static esp_err_t h_advanced_page(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    return send_asset(req, "text/html", advanced_html_start, advanced_html_end);
}

static esp_err_t h_api_valves(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    int num_valves = config_get().num_valves;
    char resp[MAX_VALVES * 8 + 1];
    int pos = 0;
    for (int i = 0; i < num_valves; i++) {
        pos += snprintf(resp + pos, sizeof(resp) - pos, "%d:%d\n", i, valve_is_on(i) ? 1 : 0);
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, pos);
    return ESP_OK;
}

static esp_err_t h_api_valve_set(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    char query[64];
    int idx = -1;
    bool on = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "idx", val, sizeof(val)) == ESP_OK) {
            idx = atoi(val);
        }
        if (httpd_query_key_value(query, "on", val, sizeof(val)) == ESP_OK) {
            on = atoi(val) != 0;
        }
    }

    if (idx < 0 || idx >= config_get().num_valves) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad valve index");
        return ESP_FAIL;
    }

    valve_set(idx, on);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t h_api_settings(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    const app_config_t &cfg = config_get();
    char resp[256];
    int pos = snprintf(resp, sizeof(resp),
                        "device_name=%s\nmqtt_uri=%s\nmqtt_user=%s\n",
                        cfg.device_name, cfg.mqtt_uri, cfg.mqtt_user);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, pos);
    return ESP_OK;
}

static esp_err_t h_settings_save(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    char body[512];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    app_config_t cfg = config_get();
    char field[64];

    if (form_get(body, "device_name", field, sizeof(field)) && field[0] != '\0') {
        strlcpy(cfg.device_name, field, sizeof(cfg.device_name));
    }
    if (form_get(body, "mqtt_uri", field, sizeof(field))) {
        strlcpy(cfg.mqtt_uri, field, sizeof(cfg.mqtt_uri));
    }
    if (form_get(body, "mqtt_user", field, sizeof(field))) {
        strlcpy(cfg.mqtt_user, field, sizeof(cfg.mqtt_user));
    }
    // Blank password fields mean "keep the current value" -- they're never
    // echoed back by GET /api/settings, so blank can't mean "clear it".
    if (form_get(body, "mqtt_pass", field, sizeof(field)) && field[0] != '\0') {
        strlcpy(cfg.mqtt_pass, field, sizeof(cfg.mqtt_pass));
    }

    char disable_pw[4];
    if (form_get(body, "disable_web_password", disable_pw, sizeof(disable_pw))) {
        cfg.web_password[0] = '\0';
    } else if (form_get(body, "web_password", field, sizeof(field)) && field[0] != '\0') {
        strlcpy(cfg.web_password, field, sizeof(cfg.web_password));
    }

    config_save(cfg);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    reboot_after_response();
    return ESP_OK;
}

// GPIO_NUM_NC ("not configured") is rendered as an empty value so the web UI
// shows a blank field rather than a confusing "-1".
static int append_gpio_field(char *buf, size_t buf_size, int pos, const char *key, int gpio)
{
    if (gpio == GPIO_NUM_NC) {
        return pos + snprintf(buf + pos, buf_size - pos, "%s=\n", key);
    }
    return pos + snprintf(buf + pos, buf_size - pos, "%s=%d\n", key, gpio);
}

static esp_err_t h_api_advanced(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    const app_config_t &cfg = config_get();
    char resp[MAX_VALVES * 40 + 128];
    int pos = 0;
    pos += snprintf(resp + pos, sizeof(resp) - pos, "num_valves=%d\n", cfg.num_valves);
    pos += snprintf(resp + pos, sizeof(resp) - pos, "max_valves=%d\n", MAX_VALVES);
    // Only the currently active valves get a field -- growing num_valves and
    // saving is what reveals fields for the newly added ones (2-step flow:
    // save num_valves + restart, then the extra GPIO fields appear to fill in
    // and save again).
    for (int i = 0; i < cfg.num_valves; i++) {
        char key[16];
        snprintf(key, sizeof(key), "gpio%d", i);
        pos = append_gpio_field(resp, sizeof(resp), pos, key, cfg.valve_gpio[i]);
    }
    pos = append_gpio_field(resp, sizeof(resp), pos, "wifi_led_gpio", cfg.wifi_led_gpio);
    pos = append_gpio_field(resp, sizeof(resp), pos, "mqtt_led_gpio", cfg.mqtt_led_gpio);
    for (int i = 0; i < cfg.num_valves; i++) {
        char key[32];
        snprintf(key, sizeof(key), "valve_led_gpio%d", i);
        pos = append_gpio_field(resp, sizeof(resp), pos, key, cfg.valve_led_gpio[i]);
    }
    pos += snprintf(resp + pos, sizeof(resp) - pos, "conn_leds_enabled=%d\n", cfg.conn_leds_enabled ? 1 : 0);
    pos += snprintf(resp + pos, sizeof(resp) - pos, "valve_leds_enabled=%d\n", cfg.valve_leds_enabled ? 1 : 0);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, pos);
    return ESP_OK;
}

// A field missing from the body entirely is left untouched (it simply
// wasn't rendered on the page this save came from, e.g. a valve slot added
// by this same save via num_valves that has no GPIO field yet). A field
// present but blank means "not configured" (GPIO_NUM_NC).
static void apply_nullable_gpio(const char *body, const char *key, int *field)
{
    char value[8];
    esp_err_t err = httpd_query_key_value(body, key, value, sizeof(value));
    if (err == ESP_ERR_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK || value[0] == '\0') {
        *field = GPIO_NUM_NC;
        return;
    }
    *field = atoi(value);
}

static esp_err_t h_advanced_save(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    char body[512];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    app_config_t cfg = config_get();

    char nv[8];
    if (httpd_query_key_value(body, "num_valves", nv, sizeof(nv)) == ESP_OK && nv[0] != '\0') {
        int n = atoi(nv);
        if (n < 1) n = 1;
        if (n > MAX_VALVES) n = MAX_VALVES;
        cfg.num_valves = n;
    }

    // Loop over MAX_VALVES (not num_valves, old or new) so any GPIO field
    // actually present in the body gets applied -- see apply_nullable_gpio.
    for (int i = 0; i < MAX_VALVES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "gpio%d", i);
        apply_nullable_gpio(body, key, &cfg.valve_gpio[i]);
    }

    apply_nullable_gpio(body, "wifi_led_gpio", &cfg.wifi_led_gpio);
    apply_nullable_gpio(body, "mqtt_led_gpio", &cfg.mqtt_led_gpio);
    for (int i = 0; i < MAX_VALVES; i++) {
        char key[32];
        snprintf(key, sizeof(key), "valve_led_gpio%d", i);
        apply_nullable_gpio(body, key, &cfg.valve_led_gpio[i]);
    }

    // Checkboxes only appear in the body when checked -- same convention as
    // "disable_web_password" on the Settings page.
    char cb[4];
    cfg.conn_leds_enabled = httpd_query_key_value(body, "disable_conn_leds", cb, sizeof(cb)) != ESP_OK;
    cfg.valve_leds_enabled = httpd_query_key_value(body, "disable_valve_leds", cb, sizeof(cb)) != ESP_OK;

    config_save(cfg);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    reboot_after_response();
    return ESP_OK;
}

static esp_err_t h_reset_wifi(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    config_clear_wifi();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    reboot_after_response();
    return ESP_OK;
}

static esp_err_t h_style_css(httpd_req_t *req)
{
    return send_asset(req, "text/css", style_css_start, style_css_end);
}

void web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    if (net_setup_in_ap_mode()) {
        static const httpd_uri_t handlers[] = {
            { .uri = "/", .method = HTTP_GET, .handler = h_setup_page, .user_ctx = NULL },
            { .uri = "/style.css", .method = HTTP_GET, .handler = h_style_css, .user_ctx = NULL },
            { .uri = "/api/scan", .method = HTTP_GET, .handler = h_api_scan, .user_ctx = NULL },
            { .uri = "/save-wifi", .method = HTTP_POST, .handler = h_save_wifi, .user_ctx = NULL },
        };
        for (const auto &h : handlers) {
            httpd_register_uri_handler(server, &h);
        }
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, h_captive_catchall);
        ESP_LOGI(TAG, "Web server up in setup mode");
    } else {
        static const httpd_uri_t handlers[] = {
            { .uri = "/", .method = HTTP_GET, .handler = h_index_page, .user_ctx = NULL },
            { .uri = "/style.css", .method = HTTP_GET, .handler = h_style_css, .user_ctx = NULL },
            { .uri = "/api/valves", .method = HTTP_GET, .handler = h_api_valves, .user_ctx = NULL },
            { .uri = "/api/valve", .method = HTTP_POST, .handler = h_api_valve_set, .user_ctx = NULL },
            { .uri = "/settings", .method = HTTP_GET, .handler = h_settings_page, .user_ctx = NULL },
            { .uri = "/settings", .method = HTTP_POST, .handler = h_settings_save, .user_ctx = NULL },
            { .uri = "/settings/advanced", .method = HTTP_GET, .handler = h_advanced_page, .user_ctx = NULL },
            { .uri = "/settings/advanced", .method = HTTP_POST, .handler = h_advanced_save, .user_ctx = NULL },
            { .uri = "/settings/reset-wifi", .method = HTTP_POST, .handler = h_reset_wifi, .user_ctx = NULL },
            { .uri = "/api/settings", .method = HTTP_GET, .handler = h_api_settings, .user_ctx = NULL },
            { .uri = "/api/settings/advanced", .method = HTTP_GET, .handler = h_api_advanced, .user_ctx = NULL },
        };
        for (const auto &h : handlers) {
            httpd_register_uri_handler(server, &h);
        }
        ESP_LOGI(TAG, "Web server up in normal mode");
    }
}
