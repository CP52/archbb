#pragma once
#include <cstdint>
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;
int esp_read_mac(uint8_t*, esp_mac_type_t);
