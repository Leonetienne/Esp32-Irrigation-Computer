#include "leds.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "config_store.h"

#define MQTT_PULSE_US (60 * 1000)

static gpio_num_t s_wifi_led = GPIO_NUM_NC;
static gpio_num_t s_mqtt_led = GPIO_NUM_NC;
static gpio_num_t s_valve_led[MAX_VALVES];
static bool s_conn_leds_enabled;
static bool s_valve_leds_enabled;
static esp_timer_handle_t s_mqtt_pulse_timer = NULL;

static void init_led_pin(gpio_num_t gpio)
{
    if (gpio == GPIO_NUM_NC) {
        return;
    }
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, 0);
}

static void mqtt_pulse_timer_cb(void *arg)
{
    if (s_conn_leds_enabled && s_mqtt_led != GPIO_NUM_NC) {
        gpio_set_level(s_mqtt_led, 1);
    }
}

void led_init(void)
{
    const app_config_t &cfg = config_get();

    s_conn_leds_enabled = cfg.conn_leds_enabled;
    s_valve_leds_enabled = cfg.valve_leds_enabled;

    s_wifi_led = (gpio_num_t)cfg.wifi_led_gpio;
    s_mqtt_led = (gpio_num_t)cfg.mqtt_led_gpio;
    init_led_pin(s_wifi_led);
    init_led_pin(s_mqtt_led);

    if (s_mqtt_led != GPIO_NUM_NC) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = mqtt_pulse_timer_cb;
        timer_args.name = "mqtt_led_pulse";
        esp_timer_create(&timer_args, &s_mqtt_pulse_timer);
    }

    // Initialize all MAX_VALVES slots regardless of the current num_valves --
    // GPIO assignments for temporarily-unused valve slots stay harmless
    // (GPIO_NUM_NC, skipped by init_led_pin) and are preserved either way.
    for (int i = 0; i < MAX_VALVES; i++) {
        s_valve_led[i] = (gpio_num_t)cfg.valve_led_gpio[i];
        init_led_pin(s_valve_led[i]);
    }
}

// Disabling a group leaves its GPIO config untouched in NVS -- the pin is
// simply never driven high, so the LED stays dark without losing the wiring.

void led_set_wifi(bool on)
{
    if (s_conn_leds_enabled && s_wifi_led != GPIO_NUM_NC) {
        gpio_set_level(s_wifi_led, on ? 1 : 0);
    }
}

void led_set_mqtt(bool on)
{
    if (!s_conn_leds_enabled || s_mqtt_led == GPIO_NUM_NC) {
        return;
    }
    if (!on && s_mqtt_pulse_timer != NULL && esp_timer_is_active(s_mqtt_pulse_timer)) {
        // Went offline mid-blink -- cancel the pending "turn back on" or
        // it would relight the LED right after we just turned it off.
        esp_timer_stop(s_mqtt_pulse_timer);
    }
    gpio_set_level(s_mqtt_led, on ? 1 : 0);
}

void led_pulse_mqtt(void)
{
    if (!s_conn_leds_enabled || s_mqtt_led == GPIO_NUM_NC || s_mqtt_pulse_timer == NULL) {
        return;
    }
    gpio_set_level(s_mqtt_led, 0);
    if (esp_timer_is_active(s_mqtt_pulse_timer)) {
        esp_timer_restart(s_mqtt_pulse_timer, MQTT_PULSE_US);
    } else {
        esp_timer_start_once(s_mqtt_pulse_timer, MQTT_PULSE_US);
    }
}

void led_set_valve(int idx, bool on)
{
    if (s_valve_leds_enabled && s_valve_led[idx] != GPIO_NUM_NC) {
        gpio_set_level(s_valve_led[idx], on ? 1 : 0);
    }
}
