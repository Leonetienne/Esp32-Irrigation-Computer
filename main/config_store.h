#pragma once

#include "driver/gpio.h"

// Hard ceiling on valves this firmware build can ever drive -- array storage
// below is sized to this at compile time. The number actually in use is
// runtime-configurable (up to this cap) via the web UI's advanced settings
// page, as app_config_t::num_valves.
#define MAX_VALVES 8

struct app_config_t {
    char device_name[32];
    char wifi_ssid[32];
    char wifi_pass[64];
    char mqtt_uri[64];
    char mqtt_user[32];
    char mqtt_pass[64];
    char web_password[32];

    // MQTT discovery/ACL namespace shared across a fleet of controllers --
    // see the comment on topic_discovery() in main.cpp.
    char node_id[32];

    // Safety failsafe: force a valve off if it's been on continuously this
    // long, in case a "close" command from HA never arrives.
    bool runtime_safety_enabled;
    int max_valve_runtime_sec;

    // Safety failsafe: shut all valves off if connectivity drops, since a
    // "close" command can't reach the device anymore either way.
    bool cut_on_wifi_loss;
    bool cut_on_mqtt_loss;

    int num_valves;
    int valve_gpio[MAX_VALVES];

    // Status LED GPIOs. GPIO_NUM_NC (-1) means "not configured" -- that LED,
    // or valve GPIO beyond num_valves, is left alone entirely, no pin touched.
    int wifi_led_gpio;
    int mqtt_led_gpio;
    int valve_led_gpio[MAX_VALVES];

    // Master on/off switches for the LED groups above -- lets a user turn
    // all connectivity or all valve status LEDs off without erasing the
    // GPIO numbers they already set up.
    bool conn_leds_enabled;
    bool valve_leds_enabled;
};

// Loads persisted config from NVS into an in-memory copy, filling in
// defaults for anything never saved. Call once at boot before config_get().
void config_store_init(void);

const app_config_t &config_get(void);

// Persists the given config to NVS and updates the in-memory copy.
void config_save(const app_config_t &cfg);

// Erases only the stored WiFi credentials (used for the "forget WiFi" /
// long-press-button re-setup flow) without touching other settings.
void config_clear_wifi(void);

bool config_has_wifi(void);
bool config_has_mqtt(void);
