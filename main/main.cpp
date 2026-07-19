#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_timer.h"

#include "config_store.h"
#include "net_setup.h"
#include "web_server.h"
#include "valves.h"
#include "leds.h"

#define PAYLOAD_ON  "ON"
#define PAYLOAD_OFF "OFF"

#define BUTTON_GPIO GPIO_NUM_0   // onboard BOOT button, active low

// Hold the BOOT button for this long right at power-on to wipe the stored
// WiFi credentials and drop back into setup-AP mode.
#define WIFI_RESET_HOLD_MS 5000

static const char *TAG = "irrigation";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;
static volatile bool s_valve_on[MAX_VALVES];
static int64_t s_valve_on_since_us[MAX_VALVES];
static gpio_num_t s_valve_gpio[MAX_VALVES];
static int s_num_valves;
static char s_avail_topic[64];

#define TOPIC_LEN 96

static void topic_cmnd(int idx, char *buf, size_t len)
{
    snprintf(buf, len, "cmnd/irrigation/%s/%d/POWER", config_get().device_name, idx);
}

static void topic_stat(int idx, char *buf, size_t len)
{
    snprintf(buf, len, "stat/irrigation/%s/%d/POWER", config_get().device_name, idx);
}

static void topic_discovery(int idx, char *buf, size_t len)
{
    snprintf(buf, len, "homeassistant/switch/%s/%s_%d/config", config_get().node_id, config_get().device_name, idx);
}

// All outgoing publishes go through here so the MQTT LED blinks on every one.
static void mqtt_publish(const char *topic, const char *data, int qos, bool retain)
{
    esp_mqtt_client_publish(s_mqtt_client, topic, data, 0, qos, retain);
    led_pulse_mqtt();
}

static void publish_state(int idx)
{
    if (!s_mqtt_connected || s_mqtt_client == NULL) {
        return;
    }
    char topic[TOPIC_LEN];
    topic_stat(idx, topic, sizeof(topic));
    mqtt_publish(topic, s_valve_on[idx] ? PAYLOAD_ON : PAYLOAD_OFF, 1, true);
}

static void set_valve_state(int idx, bool on)
{
    s_valve_on[idx] = on;
    if (on) {
        s_valve_on_since_us[idx] = esp_timer_get_time();
    }
    if (s_valve_gpio[idx] != GPIO_NUM_NC) {
        gpio_set_level(s_valve_gpio[idx], on ? 1 : 0);
    }
    led_set_valve(idx, on);
    publish_state(idx);
}

bool valve_is_on(int idx)
{
    return s_valve_on[idx];
}

void valve_set(int idx, bool on)
{
    set_valve_state(idx, on);
}

static void shutoff_all_valves(const char *reason)
{
    for (int i = 0; i < s_num_valves; i++) {
        if (s_valve_on[i]) {
            ESP_LOGW(TAG, "%s -- shutting off valve %d", reason, i);
            set_valve_state(i, false);
        }
    }
}

static void check_runtime_safety(void)
{
    const app_config_t &cfg = config_get();
    if (!cfg.runtime_safety_enabled) {
        return;
    }
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_num_valves; i++) {
        if (s_valve_on[i] &&
            (now - s_valve_on_since_us[i]) >= (int64_t)cfg.max_valve_runtime_sec * 1000000LL) {
            ESP_LOGW(TAG, "Valve %d exceeded max runtime of %d s -- forcing off", i, cfg.max_valve_runtime_sec);
            set_valve_state(i, false);
        }
    }
}

static void publish_discovery(int idx)
{
    const char *device_id = config_get().device_name;
    const char *node_id = config_get().node_id;

    char cmnd_t[TOPIC_LEN], stat_t[TOPIC_LEN], discovery_t[TOPIC_LEN];
    topic_cmnd(idx, cmnd_t, sizeof(cmnd_t));
    topic_stat(idx, stat_t, sizeof(stat_t));
    topic_discovery(idx, discovery_t, sizeof(discovery_t));

    char payload[900];
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s Valve %d\","
        "\"unique_id\":\"%s_%s_%d\","
        "\"state_topic\":\"%s\","
        "\"command_topic\":\"%s\","
        "\"payload_on\":\"" PAYLOAD_ON "\","
        "\"payload_off\":\"" PAYLOAD_OFF "\","
        "\"state_on\":\"" PAYLOAD_ON "\","
        "\"state_off\":\"" PAYLOAD_OFF "\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"Online\","
        "\"payload_not_available\":\"Offline\","
        "\"device\":{"
            "\"identifiers\":[\"%s_%s\"],"
            "\"name\":\"%s\","
            "\"manufacturer\":\"Leonetienne\","
            "\"model\":\"ESP32 Irrigation Controller\""
        "}"
        "}",
        device_id, idx,
        node_id, device_id, idx,
        stat_t, cmnd_t,
        s_avail_topic,
        node_id, device_id,
        device_id);

    mqtt_publish(discovery_t, payload, 1, true);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_connected = true;
        led_set_mqtt(true);
        mqtt_publish(s_avail_topic, "Online", 1, true);
        for (int i = 0; i < s_num_valves; i++) {
            char cmnd_t[TOPIC_LEN];
            topic_cmnd(i, cmnd_t, sizeof(cmnd_t));
            esp_mqtt_client_subscribe(s_mqtt_client, cmnd_t, 1);
            publish_discovery(i);
            publish_state(i);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        led_set_mqtt(false);
        if (config_get().cut_on_mqtt_loss) {
            shutoff_all_valves("MQTT disconnected");
        }
        break;

    case MQTT_EVENT_DATA:
        led_pulse_mqtt();
        for (int i = 0; i < s_num_valves; i++) {
            char cmnd_t[TOPIC_LEN];
            topic_cmnd(i, cmnd_t, sizeof(cmnd_t));
            if (event->topic_len == (int)strlen(cmnd_t) &&
                strncmp(event->topic, cmnd_t, event->topic_len) == 0) {
                if (event->data_len == (int)strlen(PAYLOAD_ON) &&
                    strncmp(event->data, PAYLOAD_ON, event->data_len) == 0) {
                    set_valve_state(i, true);
                } else if (event->data_len == (int)strlen(PAYLOAD_OFF) &&
                           strncmp(event->data, PAYLOAD_OFF, event->data_len) == 0) {
                    set_valve_state(i, false);
                }
                break;
            }
        }
        break;

    default:
        break;
    }
}

static void mqtt_start(void)
{
    const app_config_t &cfg = config_get();
    snprintf(s_avail_topic, sizeof(s_avail_topic), "tele/irrigation/%s/LWT", cfg.device_name);

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = cfg.mqtt_uri;
    mqtt_cfg.credentials.username = cfg.mqtt_user;
    mqtt_cfg.credentials.authentication.password = cfg.mqtt_pass;
    mqtt_cfg.session.last_will.topic = s_avail_topic;
    mqtt_cfg.session.last_will.msg = "Offline";
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = true;

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

static void on_wifi_connected(void)
{
    if (s_mqtt_client == NULL && config_has_mqtt()) {
        mqtt_start();
    }
}

static void on_wifi_disconnected(void)
{
    s_mqtt_connected = false;
    led_set_mqtt(false);
    if (config_get().cut_on_wifi_loss) {
        shutoff_all_valves("WiFi disconnected");
    }
}

static void log_wifi_rssi_periodic(void)
{
    static int64_t last_log_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_log_us < 5000000) {
        return;
    }
    last_log_us = now;

    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi RSSI: %d dBm (channel %d)", info.rssi, info.primary);
    }
}

// Holding the BOOT button down through power-on for WIFI_RESET_HOLD_MS wipes
// the stored WiFi credentials, so net_setup_start() falls straight into
// setup-AP mode afterwards.
static void check_wifi_reset_button(void)
{
    if (gpio_get_level(BUTTON_GPIO) != 0) {
        return;
    }
    ESP_LOGI(TAG, "Button held at boot -- hold %d ms to reset WiFi", WIFI_RESET_HOLD_MS);
    for (int waited = 0; waited < WIFI_RESET_HOLD_MS; waited += 100) {
        if (gpio_get_level(BUTTON_GPIO) != 0) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "WiFi credentials reset via button long-press");
    config_clear_wifi();
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    config_store_init();

    const app_config_t &cfg = config_get();
    s_num_valves = cfg.num_valves;
    for (int i = 0; i < s_num_valves; i++) {
        s_valve_gpio[i] = (gpio_num_t)cfg.valve_gpio[i];
        if (s_valve_gpio[i] == GPIO_NUM_NC) {
            // Newly added valve slot -- not wired up yet. Skip until the
            // GPIO is configured on the Advanced settings page.
            continue;
        }
        gpio_reset_pin(s_valve_gpio[i]);
        gpio_set_direction(s_valve_gpio[i], GPIO_MODE_OUTPUT);
        gpio_set_level(s_valve_gpio[i], 0);
    }

    led_init();

    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);

    check_wifi_reset_button();

    net_setup_register_callbacks(on_wifi_connected, on_wifi_disconnected);
    net_setup_start();
    web_server_start();

    int last_button = 1; // not pressed (pulled up)

    while (1) {
        int button = gpio_get_level(BUTTON_GPIO);

        if (last_button == 1 && button == 0) {
            // falling edge = manual override on valve 0, let HA know
            set_valve_state(0, !s_valve_on[0]);
            vTaskDelay(pdMS_TO_TICKS(50)); // debounce
        }

        last_button = button;
        check_runtime_safety();
        log_wifi_rssi_periodic();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
