#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_timer.h"

#define WIFI_SSID "strictbembel_optout_nomap"
#define WIFI_PASS "***REMOVED-WIFI-PASSWORD***"

#define MQTT_URI  "mqtt://192.168.0.14:1883"
#define MQTT_USER "garden_devices"
#define MQTT_PASS "***REMOVED-MQTT-PASSWORD***"

// MQTT discovery "node" all garden devices share -- must match the ACL grant
// on the broker (homeassistant/switch/garden_devices/#).
#define NODE_ID "garden_devices"

// Identifies this physical controller. Give each controller a unique,
// hardcoded ID before flashing (e.g. "GARAGE_SPRINKLER", "BACKYARD_DRIP").
#define CONTROLLER_ID "GARAGE_SPRINKLER"

#define PAYLOAD_ON  "ON"
#define PAYLOAD_OFF "OFF"

#define BUTTON_GPIO GPIO_NUM_0   // onboard BOOT button, active low

// Safety failsafe: force a valve off if it's been continuously on this long,
// in case a "close" command from HA never arrives. Set the _ENABLED flag to
// false for a controller that legitimately needs longer runs.
#define MAX_RUNTIME_SAFETY_ENABLED true
#define MAX_VALVE_RUNTIME_SEC (6 * 60 * 60)

// One entry per valve this controller drives. HA gets one switch entity per
// entry, addressed by its index in this array. To add valves, add rows here
// (and matching relay wiring) -- no broker/ACL changes needed, the ACL
// grants are already scoped to the whole controller, not individual valves.
struct valve_t {
    gpio_num_t gpio;
};

static const valve_t VALVES[] = {
    { GPIO_NUM_23 }, // index 0
};
#define NUM_VALVES (sizeof(VALVES) / sizeof(VALVES[0]))

static const char *TAG = "irrigation";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;
static volatile bool s_valve_on[NUM_VALVES];
static int64_t s_valve_on_since_us[NUM_VALVES];
static char s_avail_topic[64];

#define TOPIC_LEN 96

static void topic_cmnd(int idx, char *buf, size_t len)
{
    snprintf(buf, len, "cmnd/irrigation/%s/%d/POWER", CONTROLLER_ID, idx);
}

static void topic_stat(int idx, char *buf, size_t len)
{
    snprintf(buf, len, "stat/irrigation/%s/%d/POWER", CONTROLLER_ID, idx);
}

static void topic_discovery(int idx, char *buf, size_t len)
{
    snprintf(buf, len, "homeassistant/switch/" NODE_ID "/%s_%d/config", CONTROLLER_ID, idx);
}

static void publish_state(int idx)
{
    if (!s_mqtt_connected || s_mqtt_client == NULL) {
        return;
    }
    char topic[TOPIC_LEN];
    topic_stat(idx, topic, sizeof(topic));
    esp_mqtt_client_publish(s_mqtt_client, topic,
                             s_valve_on[idx] ? PAYLOAD_ON : PAYLOAD_OFF,
                             0, 1, true);
}

static void set_valve_state(int idx, bool on)
{
    s_valve_on[idx] = on;
    if (on) {
        s_valve_on_since_us[idx] = esp_timer_get_time();
    }
    gpio_set_level(VALVES[idx].gpio, on ? 1 : 0);
    publish_state(idx);
}

static void shutoff_all_valves(const char *reason)
{
    for (int i = 0; i < (int)NUM_VALVES; i++) {
        if (s_valve_on[i]) {
            ESP_LOGW(TAG, "%s -- shutting off valve %d", reason, i);
            set_valve_state(i, false);
        }
    }
}

static void check_runtime_safety(void)
{
    if (!MAX_RUNTIME_SAFETY_ENABLED) {
        return;
    }
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < (int)NUM_VALVES; i++) {
        if (s_valve_on[i] &&
            (now - s_valve_on_since_us[i]) >= (int64_t)MAX_VALVE_RUNTIME_SEC * 1000000LL) {
            ESP_LOGW(TAG, "Valve %d exceeded max runtime of %d s -- forcing off", i, MAX_VALVE_RUNTIME_SEC);
            set_valve_state(i, false);
        }
    }
}

static void publish_discovery(int idx)
{
    char cmnd_t[TOPIC_LEN], stat_t[TOPIC_LEN], discovery_t[TOPIC_LEN];
    topic_cmnd(idx, cmnd_t, sizeof(cmnd_t));
    topic_stat(idx, stat_t, sizeof(stat_t));
    topic_discovery(idx, discovery_t, sizeof(discovery_t));

    char payload[768];
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s Valve %d\","
        "\"unique_id\":\"" NODE_ID "_%s_%d\","
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
            "\"identifiers\":[\"" NODE_ID "_%s\"],"
            "\"name\":\"%s\","
            "\"manufacturer\":\"DIY\","
            "\"model\":\"ESP32 Irrigation Controller\""
        "}"
        "}",
        CONTROLLER_ID, idx,
        CONTROLLER_ID, idx,
        stat_t, cmnd_t,
        s_avail_topic,
        CONTROLLER_ID,
        CONTROLLER_ID);

    esp_mqtt_client_publish(s_mqtt_client, discovery_t, payload, 0, 1, true);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_connected = true;
        esp_mqtt_client_publish(s_mqtt_client, s_avail_topic, "Online", 0, 1, true);
        for (int i = 0; i < (int)NUM_VALVES; i++) {
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
        shutoff_all_valves("MQTT disconnected");
        break;

    case MQTT_EVENT_DATA:
        for (int i = 0; i < (int)NUM_VALVES; i++) {
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
    snprintf(s_avail_topic, sizeof(s_avail_topic), "tele/irrigation/%s/LWT", CONTROLLER_ID);

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = MQTT_URI;
    mqtt_cfg.credentials.username = MQTT_USER;
    mqtt_cfg.credentials.authentication.password = MQTT_PASS;
    mqtt_cfg.session.last_will.topic = s_avail_topic;
    mqtt_cfg.session.last_will.msg = "Offline";
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = true;

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying indefinitely");
        s_mqtt_connected = false;
        shutoff_all_valves("WiFi disconnected");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi got IP");
        if (s_mqtt_client == NULL) {
            mqtt_start();
        }
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Mains-powered, latency/reliability matters more than idle power draw --
    // modem sleep power-save tends to make weak-signal links flakier.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
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

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    for (int i = 0; i < (int)NUM_VALVES; i++) {
        gpio_reset_pin(VALVES[i].gpio);
        gpio_set_direction(VALVES[i].gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(VALVES[i].gpio, 0);
    }

    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);

    wifi_init();

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
