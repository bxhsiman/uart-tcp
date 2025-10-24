#pragma once

// #define CLEAN_NVS

/* Wi‑Fi STA 信息 */
#define WIFI_SSID           "miwifi"
#define WIFI_PASS           "(12345678)"
#define WIFI_MAX_RETRY      9999
#define WIFI_TX_POWER       44
//   * @attention 3. Mapping Table {Power, max_tx_power} = {{8,   2}, {20,  5}, {28,  7}, {34,  8}, {44, 11},
//   *                                                      {52, 13}, {56, 14}, {60, 15}, {66, 16}, {72, 18}, {80, 20}}.

/* WiFi管理配置 */
#define ENABLE_SOFTAP       1                    // 1=开启SoftAP, 0=关闭
#define SOFTAP_SSID_PREFIX  "ESP_"               // SoftAP SSID前缀
#define SOFTAP_PASSWORD     "12345678"          // SoftAP密码
#define SOFTAP_MAX_CONN     4                   // 最大连接数
#define HTTP_SERVER_PORT    80                  // HTTP服务器端口

/* HTTP服务器缓冲区配置 */
#define HTTP_MAX_URI_LEN    1024                // HTTP URI最大长度
#define HTTP_MAX_HEADER_LEN 2048                // HTTP请求头最大长度
#define HTTP_RECV_BUF_SIZE  2048                // HTTP接收缓冲区大小

/* UART 参数 */
#define UART_PORT_NUM       UART_NUM_1
#define UART_BAUD_RATE      921600
#define UART_TX_PIN         1
#define UART_RX_PIN         2
#define UART_BUF_SIZE       2048

/* ■ Client 模式专用宏 */
#define REMOTE_SERVER_IP    "192.168.89.46"   // ★ 改成你的服务器 IP
#define REMOTE_SERVER_PORT  6001               // ★ 改成你的服务器端口
#define TCP_RECONNECT_MS    500                // ★ 断线后重新连接间隔

/* 公共缓冲区 */
#define TCP_RECV_BUF_SIZE   2048

/* LiDAR数据包格式 */
#define LIDAR_PACKET_SIZE   44
#define LIDAR_FRAME_PACKETS 8
#define LIDAR_FRAME_SIZE    (LIDAR_PACKET_SIZE * LIDAR_FRAME_PACKETS)  // 352 bytes
#define LIDAR_HEADER_0      0x0A
#define LIDAR_HEADER_1      0x00
#define FRAME_BUFFER_COUNT  20   // 缓冲多少帧再发送


/* LED状态控制 */
#define LED_PERIOD_NORMAL   1000    // 正常工作时LED闪烁周期 (ms)
#define LED_PERIOD_ERROR    200     // 出错时LED闪烁周期 (ms)

/* 日志控制宏 */
#define ENABLE_DEBUG_LOG    0   // 1=开启详细调试日志, 0=关闭
#define ENABLE_INFO_LOG     0   // 1=开启信息日志, 0=关闭  
#define ENABLE_WARN_LOG     0   // 1=开启警告日志, 0=关闭
#define ENABLE_ERROR_LOG    1   // 1=开启错误日志, 0=关闭

/* 可控日志宏定义 */
#if ENABLE_DEBUG_LOG
    #define LOG_D(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#else
    #define LOG_D(tag, format, ...) do { } while(0)
#endif

#if ENABLE_INFO_LOG
    #define LOG_I(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#else
    #define LOG_I(tag, format, ...) do { } while(0)
#endif

#if ENABLE_WARN_LOG
    #define LOG_W(tag, format, ...) ESP_LOGW(tag, format, ##__VA_ARGS__)
#else
    #define LOG_W(tag, format, ...) do { } while(0)
#endif

#if ENABLE_ERROR_LOG
    #define LOG_E(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)
#else
    #define LOG_E(tag, format, ...) do { } while(0)
#endif