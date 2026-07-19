#pragma once

// Thin interface main.cpp exposes so web_server.cpp can read/drive valve
// state without depending on the MQTT/discovery internals.
bool valve_is_on(int idx);
void valve_set(int idx, bool on);
