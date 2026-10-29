// SPDX-License-Identifier: MIT
#pragma once
#include "mqtt_bridge.h"
void mqtt_discovery_init(const char *bridge_id);
esp_err_t mqtt_discovery_set(bool enabled);
void mqtt_discovery_replay(void);
void mqtt_discovery_disconnected(void);
void mqtt_discovery_tick(mqtt_client_ops *client);
void mqtt_discovery_status(mqtt_bridge_status *out);
bool mqtt_discovery_has_records(void);
