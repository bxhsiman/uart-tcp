#pragma once

/* Wi‑Fi STA 信息 */
#define WIFI_SSID           "Xiaomi_7E5B"
#define WIFI_PASS           "richbeam"
#define WIFI_MAX_RETRY      5

/* UART 参数 */
#define UART_PORT_NUM       UART_NUM_1
#define UART_BAUD_RATE      921600
#define UART_TX_PIN         3
#define UART_RX_PIN         4
#define UART_BUF_SIZE       2048

/* ■ Client 模式专用宏 */
#define REMOTE_SERVER_IP    "192.168.114.117"   // ★ 改成你的服务器 IP
#define REMOTE_SERVER_PORT  3334                // ★ 改成你的服务器端口
#define TCP_RECONNECT_MS    5000                // ★ 断线后重新连接间隔

/* 公共缓冲区 */
#define TCP_RECV_BUF_SIZE   2048

/* LiDAR数据包格式 */
#define LIDAR_PACKET_SIZE   44
#define LIDAR_FRAME_PACKETS 8
#define LIDAR_FRAME_SIZE    (LIDAR_PACKET_SIZE * LIDAR_FRAME_PACKETS)  // 352 bytes
#define LIDAR_HEADER_0      0x0A
#define LIDAR_HEADER_1      0x00
#define FRAME_BUFFER_COUNT  2   // 缓冲多少帧再发送