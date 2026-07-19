#include "config_store.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "config_store";
static const char *NVS_NS = "irrigation";

static app_config_t s_config;

static void set_defaults(app_config_t &cfg)
{
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.device_name, "irrigation-controller", sizeof(cfg.device_name));

    cfg.num_valves = 1;
    cfg.valve_gpio[0] = GPIO_NUM_23;
    for (int i = 1; i < MAX_VALVES; i++) {
        cfg.valve_gpio[i] = GPIO_NUM_NC;
    }

    cfg.wifi_led_gpio = GPIO_NUM_NC;
    cfg.mqtt_led_gpio = GPIO_NUM_NC;
    for (int i = 0; i < MAX_VALVES; i++) {
        cfg.valve_led_gpio[i] = GPIO_NUM_NC;
    }

    cfg.conn_leds_enabled = true;
    cfg.valve_leds_enabled = true;
}

static void load_str(nvs_handle_t h, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load '%s': %s", key, esp_err_to_name(err));
    }
}

void config_store_init(void)
{
    set_defaults(s_config);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No stored config yet, using defaults");
        return;
    }

    load_str(h, "dev_name", s_config.device_name, sizeof(s_config.device_name));
    load_str(h, "wifi_ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
    load_str(h, "wifi_pass", s_config.wifi_pass, sizeof(s_config.wifi_pass));
    load_str(h, "mqtt_uri", s_config.mqtt_uri, sizeof(s_config.mqtt_uri));
    load_str(h, "mqtt_user", s_config.mqtt_user, sizeof(s_config.mqtt_user));
    load_str(h, "mqtt_pass", s_config.mqtt_pass, sizeof(s_config.mqtt_pass));
    load_str(h, "web_pass", s_config.web_password, sizeof(s_config.web_password));

    int32_t v;
    if (nvs_get_i32(h, "num_valves", &v) == ESP_OK) {
        if (v < 1) v = 1;
        if (v > MAX_VALVES) v = MAX_VALVES;
        s_config.num_valves = v;
    }

    // All MAX_VALVES slots are loaded regardless of num_valves, so GPIO
    // assignments survive temporarily lowering the valve count.
    for (int i = 0; i < MAX_VALVES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "valve_gpio%d", i);
        if (nvs_get_i32(h, key, &v) == ESP_OK) {
            s_config.valve_gpio[i] = v;
        }
    }

    if (nvs_get_i32(h, "wifi_led_gpio", &v) == ESP_OK) {
        s_config.wifi_led_gpio = v;
    }
    if (nvs_get_i32(h, "mqtt_led_gpio", &v) == ESP_OK) {
        s_config.mqtt_led_gpio = v;
    }
    for (int i = 0; i < MAX_VALVES; i++) {
        char key[32];
        snprintf(key, sizeof(key), "valve_led_gpio%d", i);
        if (nvs_get_i32(h, key, &v) == ESP_OK) {
            s_config.valve_led_gpio[i] = v;
        }
    }

    uint8_t b;
    if (nvs_get_u8(h, "conn_leds_on", &b) == ESP_OK) {
        s_config.conn_leds_enabled = b != 0;
    }
    if (nvs_get_u8(h, "valve_leds_on", &b) == ESP_OK) {
        s_config.valve_leds_enabled = b != 0;
    }

    nvs_close(h);
}

const app_config_t &config_get(void)
{
    return s_config;
}

void config_save(const app_config_t &cfg)
{
    s_config = cfg;

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));

    nvs_set_str(h, "dev_name", cfg.device_name);
    nvs_set_str(h, "wifi_ssid", cfg.wifi_ssid);
    nvs_set_str(h, "wifi_pass", cfg.wifi_pass);
    nvs_set_str(h, "mqtt_uri", cfg.mqtt_uri);
    nvs_set_str(h, "mqtt_user", cfg.mqtt_user);
    nvs_set_str(h, "mqtt_pass", cfg.mqtt_pass);
    nvs_set_str(h, "web_pass", cfg.web_password);

    nvs_set_i32(h, "num_valves", cfg.num_valves);
    for (int i = 0; i < MAX_VALVES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "valve_gpio%d", i);
        nvs_set_i32(h, key, cfg.valve_gpio[i]);
    }

    nvs_set_i32(h, "wifi_led_gpio", cfg.wifi_led_gpio);
    nvs_set_i32(h, "mqtt_led_gpio", cfg.mqtt_led_gpio);
    for (int i = 0; i < MAX_VALVES; i++) {
        char key[32];
        snprintf(key, sizeof(key), "valve_led_gpio%d", i);
        nvs_set_i32(h, key, cfg.valve_led_gpio[i]);
    }

    nvs_set_u8(h, "conn_leds_on", cfg.conn_leds_enabled ? 1 : 0);
    nvs_set_u8(h, "valve_leds_on", cfg.valve_leds_enabled ? 1 : 0);

    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

void config_clear_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "wifi_ssid");
        nvs_erase_key(h, "wifi_pass");
        nvs_commit(h);
        nvs_close(h);
    }
    s_config.wifi_ssid[0] = '\0';
    s_config.wifi_pass[0] = '\0';
}

bool config_has_wifi(void)
{
    return s_config.wifi_ssid[0] != '\0';
}

bool config_has_mqtt(void)
{
    return s_config.mqtt_uri[0] != '\0';
}
