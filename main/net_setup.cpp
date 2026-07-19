#include "net_setup.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "mdns.h"

#include "config_store.h"
#include "leds.h"

static const char *TAG = "net_setup";

// SoftAP always comes up on the default esp-idf AP subnet.
static const char *AP_IP = "192.168.4.1";
static const uint16_t DNS_PORT = 53;
static const TickType_t STA_CONNECT_TIMEOUT = pdMS_TO_TICKS(20000);

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;
static bool s_first_connect_done = false;
static bool s_ap_mode = false;
static void (*s_on_connected)(void) = nullptr;
static void (*s_on_disconnected)(void) = nullptr;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        led_set_wifi(false);
        // Always retry the connection itself -- a single failed association
        // attempt (common, e.g. transient AP-side rejection) shouldn't just
        // sit idle until net_setup_start()'s xEventGroupWaitBits() timeout
        // gives up. Only the app-level "we lost an established connection"
        // callback is gated on having connected at least once before.
        if (s_first_connect_done && s_on_disconnected) {
            s_on_disconnected();
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi got IP");
        s_first_connect_done = true;
        led_set_wifi(true);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_on_connected) {
            s_on_connected();
        }
    }
}

// Minimal DNS responder: answers every A-record query with the SoftAP's own
// IP so that captive-portal detection on phones/laptops pops the setup page
// automatically. Anything that isn't a simple A query is ignored.
static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint32_t ap_ip;
    inet_pton(AF_INET, AP_IP, &ap_ip);

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len < 12) {
            continue;
        }

        // Walk past the question's QNAME to find where it ends, so we know
        // where to append our answer record.
        int pos = 12;
        while (pos < len && buf[pos] != 0) {
            pos += buf[pos] + 1;
        }
        pos += 1;      // terminating zero length byte
        pos += 4;      // QTYPE + QCLASS
        if (pos > len) {
            continue;
        }

        // Flags: response, no error, recursion available.
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = 0x00; buf[7] = 0x01; // ANCOUNT = 1
        buf[8] = 0x00; buf[9] = 0x00; // NSCOUNT = 0
        buf[10] = 0x00; buf[11] = 0x00; // ARCOUNT = 0

        int reply_len = pos;
        if (reply_len + 16 > (int)sizeof(buf)) {
            continue;
        }

        buf[reply_len++] = 0xC0; buf[reply_len++] = 0x0C; // NAME: pointer to offset 12
        buf[reply_len++] = 0x00; buf[reply_len++] = 0x01; // TYPE A
        buf[reply_len++] = 0x00; buf[reply_len++] = 0x01; // CLASS IN
        buf[reply_len++] = 0x00; buf[reply_len++] = 0x00;
        buf[reply_len++] = 0x00; buf[reply_len++] = 0x3C; // TTL 60s
        buf[reply_len++] = 0x00; buf[reply_len++] = 0x04; // RDLENGTH 4
        memcpy(&buf[reply_len], &ap_ip, 4);
        reply_len += 4;

        sendto(sock, buf, reply_len, 0, (struct sockaddr *)&from, from_len);
    }
}

static void start_ap_setup_mode(void)
{
    s_ap_mode = true;

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    wifi_config_t ap_config = {};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid),
              "Irrigation-Setup-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Setup AP '%s' up at %s", ap_config.ap.ssid, AP_IP);

    xTaskCreate(dns_hijack_task, "dns_hijack", 4096, NULL, 5, NULL);
}

void net_setup_register_callbacks(void (*on_connected)(void), void (*on_disconnected)(void))
{
    s_on_connected = on_connected;
    s_on_disconnected = on_disconnected;
}

net_mode_t net_setup_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_event_group = xEventGroupCreate();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    // The WiFi driver otherwise persists its own copy of STA credentials to
    // flash independently of config_store's NVS namespace. Without this, a
    // WiFi reset (clearing config_store's copy) could still leave a stale
    // SSID/password behind that WIFI_EVENT_STA_START's esp_wifi_connect()
    // below would happily reconnect to during what's meant to be pure
    // setup-AP mode -- config_store is the single source of truth instead.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    if (!config_has_wifi()) {
        ESP_LOGI(TAG, "No WiFi credentials stored, starting setup AP");
        start_ap_setup_mode();
        return net_mode_t::AP_SETUP;
    }

    const app_config_t &cfg = config_get();

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, cfg.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, cfg.wifi_pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Mains-powered, latency/reliability matters more than idle power draw --
    // modem sleep power-save tends to make weak-signal links flakier.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, STA_CONNECT_TIMEOUT);
    if (bits & WIFI_CONNECTED_BIT) {
        // So the device is reachable as <device-name>.local instead of by IP.
        if (mdns_init() == ESP_OK) {
            mdns_hostname_set(cfg.device_name);
            mdns_instance_name_set(cfg.device_name);
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        } else {
            ESP_LOGW(TAG, "mDNS init failed, device only reachable by IP");
        }
        return net_mode_t::STA_CONNECTED;
    }

    ESP_LOGW(TAG, "Could not join stored WiFi within timeout, falling back to setup AP");
    ESP_ERROR_CHECK(esp_wifi_stop());
    esp_netif_destroy_default_wifi(sta_netif);
    start_ap_setup_mode();
    return net_mode_t::AP_SETUP;
}

bool net_setup_in_ap_mode(void)
{
    return s_ap_mode;
}

int net_setup_scan(char out[][33], int max)
{
    wifi_scan_config_t scan_cfg = {};
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return 0;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        return 0;
    }

    wifi_ap_record_t *records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * num);
    if (!records) {
        return 0;
    }
    esp_wifi_scan_get_ap_records(&num, records);

    int count = 0;
    for (int i = 0; i < (int)num && count < max; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(out[j], ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            strlcpy(out[count], ssid, 33);
            count++;
        }
    }

    free(records);
    return count;
}

const char *net_setup_ap_ip(void)
{
    return AP_IP;
}
