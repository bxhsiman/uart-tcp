#pragma once

#include "esp_http_server.h"
#include "esp_wifi.h"

/* WiFi管理统计信息 */
typedef struct {
    uint32_t total_bytes_sent;
    uint32_t total_frames_sent;
    uint32_t valid_frames;
    uint32_t invalid_frames;
    uint32_t uptime_seconds;
    bool tcp_connected;
    bool sta_connected;
} wifi_stats_t;

/* 配置结构体 */
typedef struct {
    char wifi_ssid[33];      
    char wifi_password[65];  
    char server_ip[16];      
    uint16_t server_port;    
    bool enable_softap;      
} device_config_t;

/* 函数声明 */
void init_webserver(void);
void start_softap_mode(void);
esp_err_t get_mac_address_suffix(char* suffix, size_t size);
void update_wifi_stats(wifi_stats_t* stats);
esp_err_t load_device_config(device_config_t* config);
esp_err_t init_device_config(void);  // 新增：设备配置初始化