#pragma once

// Starts the HTTP server. Registers the setup-mode handlers (WiFi scan,
// save-wifi, captive-portal catch-all) when net_setup_in_ap_mode() is true,
// otherwise the normal-mode handlers (valve control, settings). Must be
// called after net_setup_start().
void web_server_start(void);
