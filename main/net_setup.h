#pragma once

// Owns WiFi bring-up: on boot, tries to join the stored network; if that's
// missing or fails, falls back to a SoftAP + captive portal so the device
// can be configured from a phone/laptop. See config_store for credentials.

enum class net_mode_t {
    STA_CONNECTED,
    AP_SETUP,
};

// Called once from IP_EVENT_STA_GOT_IP (including reconnects after the
// initial boot). Called once from WIFI_EVENT_STA_DISCONNECTED after having
// been connected at least once (mirrors the previous inline wifi handling
// in main.cpp -- reconnects are retried forever, no automatic fallback to
// AP mode once the device has joined a network successfully).
void net_setup_register_callbacks(void (*on_connected)(void), void (*on_disconnected)(void));

// Blocking. Reads WiFi credentials from config_store and either joins that
// network (bounded ~20s timeout) or starts the SoftAP + captive portal.
net_mode_t net_setup_start(void);

bool net_setup_in_ap_mode(void);

// Scans for nearby APs (STA radio is up even in AP_SETUP mode for this).
// Fills `out` with up to `max` unique SSIDs, returns the count found.
int net_setup_scan(char out[][33], int max);

const char *net_setup_ap_ip(void);
