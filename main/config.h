#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ------------------------ 项目配置 ------------------------ */
/* 默认配置 */
#define DEFAULT_WIFI_SSID           "Xiaomi_7E5B"
#define DEFAULT_WIFI_PASS           "richbeam"
#define DEFAULT_REMOTE_SERVER_IP    "192.168.114.117"
#define DEFAULT_REMOTE_SERVER_PORT  3334
#define DEFAULT_UART_BAUD_RATE      921600

/* SoftAP 配置 */
#define SOFTAP_WIFI_SSID            "esp32"
#define SOFTAP_WIFI_PASS            "12345678"
#define SOFTAP_MAX_STA_CONN         4

#define WIFI_MAX_RETRY      5

/* UART 参数 */
#define UART_PORT_NUM       UART_NUM_1
#define UART_TX_PIN         3
#define UART_RX_PIN         4
#define UART_BUF_SIZE       2048

/* TCP重连间隔 */
#define TCP_RECONNECT_MS    5000

/* 公共缓冲区 - 优化缓冲区大小以减少丢包 */
#define TCP_RECV_BUF_SIZE   4096
#define TCP_SEND_BUF_SIZE   4096

/* LiDAR数据包检测 */
#define LIDAR_PACKET_SIZE   352
#define LIDAR_HEADER_0      0x0A
#define LIDAR_HEADER_1      0x00
#define LIDAR_BATCH_SIZE    8     /* 缓存8个序号0-7的包后批量发送 */

/* NVS存储命名空间 */
#define STORAGE_NAMESPACE   "config"

/* 配置项键名 */
#define KEY_WIFI_SSID      "wifi_ssid"
#define KEY_WIFI_PASS      "wifi_pass"
#define KEY_SERVER_IP      "server_ip"
#define KEY_SERVER_PORT    "server_port"
#define KEY_UART_BAUD      "uart_baud"
#define KEY_SOFTAP_ENABLED "softap_enabled"

/* 配置结构体 */
typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char server_ip[16];
    uint16_t server_port;
    uint32_t uart_baudrate;
    bool softap_enabled;
} config_t;

/* 配置管理函数 */
void config_init(void);
config_t* config_get(void);
esp_err_t config_save(void);
void config_load_defaults(void);

#endif // CONFIG_H