#pragma once

// Status LEDs: WiFi-OK, MQTT-OK, and one per-valve state indicator. Each is
// individually optional -- see config_store's *_led_gpio fields, GPIO_NUM_NC
// means "not wired up", and these setters silently no-op in that case.

void led_init(void);
void led_set_wifi(bool on);
void led_set_mqtt(bool on);
void led_set_valve(int idx, bool on);

// Briefly flickers the MQTT LED off and back on to indicate a message was
// just sent or received, on top of its steady "connected" state. No-op if
// disconnected/unconfigured/disabled, same as led_set_mqtt.
void led_pulse_mqtt(void);
