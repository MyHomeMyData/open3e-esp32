#ifndef STUB_ESP_MAC_H
#define STUB_ESP_MAC_H
#include <stdint.h>
typedef enum { ESP_MAC_WIFI_STA = 0 } esp_mac_type_t;
int esp_read_mac(uint8_t *out, esp_mac_type_t t);
#endif
