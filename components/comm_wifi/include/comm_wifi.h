#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    int rssi;
} CommWifiAp;

#define COMM_WIFI_MAX_CREDENTIALS 5

void comm_wifi_start(void);
void comm_wifi_stop(void);

int  comm_wifi_scan_top3(CommWifiAp* out, int cap);
bool comm_wifi_connect_psk(const char* ssid, const char* password);
bool comm_wifi_is_connected(void);
int  comm_wifi_last_disconnect_reason(void);
bool comm_wifi_connect_saved(void);
bool comm_wifi_get_connected_ssid(char* out_ssid, int cap);

int  comm_wifi_saved_credential_count(void);
bool comm_wifi_save_credential(const char* ssid, const char* password);
bool comm_wifi_connect_saved_any(uint32_t per_profile_timeout_ms, char* out_ssid, int out_cap);

bool comm_wifi_switch_to_ap_open(const char* ssid);
bool comm_wifi_switch_to_sta_only(void);
bool comm_wifi_switch_to_sta_and_reconnect(void);
bool comm_wifi_forget_saved_and_disconnect(void);

#ifdef __cplusplus
}
#endif
