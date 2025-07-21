#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "config.h"

static const char *TAG = "CONFIG";
static config_t g_config;

// Forward declaration
static void config_load_from_nvs(void);

void config_init(void)
{
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    
    // 加载配置
    config_load_from_nvs();
}

config_t* config_get(void)
{
    return &g_config;
}

static void config_load_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error opening NVS handle! Using default config");
        goto use_defaults;
    }

    size_t required_size;
    
    // 读取WiFi SSID
    required_size = sizeof(g_config.wifi_ssid);
    err = nvs_get_str(nvs_handle, KEY_WIFI_SSID, g_config.wifi_ssid, &required_size);
    if (err != ESP_OK) goto use_defaults;
    
    // 读取WiFi密码
    required_size = sizeof(g_config.wifi_pass);
    err = nvs_get_str(nvs_handle, KEY_WIFI_PASS, g_config.wifi_pass, &required_size);
    if (err != ESP_OK) goto use_defaults;
    
    // 读取服务器IP
    required_size = sizeof(g_config.server_ip);
    err = nvs_get_str(nvs_handle, KEY_SERVER_IP, g_config.server_ip, &required_size);
    if (err != ESP_OK) goto use_defaults;
    
    // 读取服务器端口
    err = nvs_get_u16(nvs_handle, KEY_SERVER_PORT, &g_config.server_port);
    if (err != ESP_OK) goto use_defaults;
    
    // 读取UART波特率
    err = nvs_get_u32(nvs_handle, KEY_UART_BAUD, &g_config.uart_baudrate);
    if (err != ESP_OK) goto use_defaults;
    
    // 读取SoftAP启用状态
    uint8_t softap_enabled = 1;  // 默认启用
    err = nvs_get_u8(nvs_handle, KEY_SOFTAP_ENABLED, &softap_enabled);
    g_config.softap_enabled = (softap_enabled != 0);
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Config loaded from NVS");
    return;
    
use_defaults:
    if (nvs_handle) nvs_close(nvs_handle);
    config_load_defaults();
}

void config_load_defaults(void)
{
    strcpy(g_config.wifi_ssid, DEFAULT_WIFI_SSID);
    strcpy(g_config.wifi_pass, DEFAULT_WIFI_PASS);
    strcpy(g_config.server_ip, DEFAULT_REMOTE_SERVER_IP);
    g_config.server_port = DEFAULT_REMOTE_SERVER_PORT;
    g_config.uart_baudrate = DEFAULT_UART_BAUD_RATE;
    g_config.softap_enabled = true;  // 默认启用SoftAP
    ESP_LOGI(TAG, "Using default config");
}

esp_err_t config_save(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle for writing!");
        return err;
    }

    err = nvs_set_str(nvs_handle, KEY_WIFI_SSID, g_config.wifi_ssid);
    if (err != ESP_OK) goto exit;
    
    err = nvs_set_str(nvs_handle, KEY_WIFI_PASS, g_config.wifi_pass);
    if (err != ESP_OK) goto exit;
    
    err = nvs_set_str(nvs_handle, KEY_SERVER_IP, g_config.server_ip);
    if (err != ESP_OK) goto exit;
    
    err = nvs_set_u16(nvs_handle, KEY_SERVER_PORT, g_config.server_port);
    if (err != ESP_OK) goto exit;
    
    err = nvs_set_u32(nvs_handle, KEY_UART_BAUD, g_config.uart_baudrate);
    if (err != ESP_OK) goto exit;
    
    uint8_t softap_enabled = g_config.softap_enabled ? 1 : 0;
    err = nvs_set_u8(nvs_handle, KEY_SOFTAP_ENABLED, softap_enabled);
    if (err != ESP_OK) goto exit;
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) goto exit;
    
    ESP_LOGI(TAG, "Config saved to NVS");

exit:
    nvs_close(nvs_handle);
    return err;
}