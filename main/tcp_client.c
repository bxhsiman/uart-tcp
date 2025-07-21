#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "tcp_client.h"
#include "config.h"

static const char *TAG = "TCP_CLIENT";

static int g_sock = -1;
static SemaphoreHandle_t g_sock_mutex;
static uart_data_callback_t uart_data_cb = NULL;

static void tcp_client_task(void *arg);
static void sock_to_uart_task(void *arg);

void tcp_client_init(void)
{
    g_sock_mutex = xSemaphoreCreateMutex();
}

void tcp_client_start_task(void)
{
    xTaskCreatePinnedToCore(tcp_client_task, "tcp_client", 8192,
                            NULL, 10, NULL, 0);  // 中等优先级，固定到核心0
}

void tcp_client_send_data(uint8_t *data, size_t len)
{
    xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
    int sock = g_sock;
    xSemaphoreGive(g_sock_mutex);
    
    if (sock < 0) {
        static uint32_t disconnected_count = 0;
        disconnected_count++;
        if (disconnected_count % 1000 == 1) {
            ESP_LOGW(TAG, "TCP not connected, dropped %lu packets", disconnected_count);
        }
        return;
    }
    
    int sent = 0;
    int total_sent = 0;
    int retry_count = 0;
    const int max_retries = 100;  // 最大重试次数
    
    // 循环发送，处理部分发送的情况
    while (total_sent < len && retry_count < max_retries) {
        sent = send(sock, data + total_sent, len - total_sent, MSG_DONTWAIT);
        if (sent > 0) {
            total_sent += sent;
            retry_count = 0;  // 重置重试计数
        } else if (sent < 0) {
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                // Socket缓冲区满，稍后重试
                retry_count++;
                if (retry_count < 10) {
                    // 短暂延迟后快速重试
                    vTaskDelay(pdMS_TO_TICKS(1));
                } else if (retry_count < 50) {
                    // 中等延迟
                    vTaskDelay(pdMS_TO_TICKS(5));
                } else {
                    // 长延迟，可能网络严重拥塞
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                continue;
            } else {
                ESP_LOGW(TAG, "Send error: %d (%s), closing connection", err, strerror(err));
                // 发送错误，关闭连接让重连机制接管
                xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
                if (g_sock == sock) {
                    close(g_sock);
                    g_sock = -1;
                }
                xSemaphoreGive(g_sock_mutex);
                break;
            }
        }
    }
    
    if (total_sent != len) {
        static uint32_t incomplete_count = 0;
        incomplete_count++;
        if (incomplete_count % 100 == 1) {
            ESP_LOGW(TAG, "Incomplete sends: %lu, last: %d/%zu bytes (retries: %d)", 
                     incomplete_count, total_sent, len, retry_count);
        }
    }
}

bool tcp_client_is_connected(void)
{
    xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
    bool connected = (g_sock >= 0);
    xSemaphoreGive(g_sock_mutex);
    return connected;
}

int tcp_client_get_socket(void)
{
    xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
    int sock = g_sock;
    xSemaphoreGive(g_sock_mutex);
    return sock;
}

SemaphoreHandle_t tcp_client_get_mutex(void)
{
    return g_sock_mutex;
}

void tcp_client_set_uart_callback(uart_data_callback_t callback)
{
    uart_data_cb = callback;
}

static void tcp_client_task(void *arg)
{
    config_t *config = config_get();
    
    for (;;) {
        /* 1. 判断是 IPv4 还是 IPv6 —— 看有没有 ':' 字符 */
        bool is_ipv6 = strchr(config->server_ip, ':');

        /* 2. 创建相应族的 socket */
        int domain = is_ipv6 ? AF_INET6 : AF_INET;
        int sock   = socket(domain, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) { ESP_LOGE(TAG, "socket() fail (%d)", errno); goto retry; }
        
        /* 2.5. 优化socket选项以确保数据可靠传输 */
        int send_buf_size = TCP_SEND_BUF_SIZE * 4;  // 进一步增大发送缓冲区
        int recv_buf_size = TCP_RECV_BUF_SIZE * 4;  // 进一步增大接收缓冲区
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size));
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recv_buf_size, sizeof(recv_buf_size));
        
        // 启用TCP_NODELAY确保数据包及时发送
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        
        // 启用SO_KEEPALIVE保持连接
        int keepalive = 1;
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        
        // 设置发送超时
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        /* 3. 填地址结构并 inet_pton */
        if (is_ipv6) {
            struct sockaddr_in6 addr6 = { 0 };
            addr6.sin6_family = AF_INET6;
            addr6.sin6_port   = htons(config->server_port);
            if (inet_pton(AF_INET6, config->server_ip, &addr6.sin6_addr) != 1) {
                ESP_LOGE(TAG, "inet_pton v6 fail");
                close(sock); goto retry;
            }
            ESP_LOGI(TAG, "Connecting to [%s]:%d ...", config->server_ip, config->server_port);
            if (connect(sock, (struct sockaddr *)&addr6, sizeof(addr6)) != 0) {
                ESP_LOGW(TAG, "connect v6 err (%d)", errno);
                close(sock); goto retry;
            }
        } else {
            struct sockaddr_in addr4 = { 0 };
            addr4.sin_family = AF_INET;
            addr4.sin_port   = htons(config->server_port);
            if (inet_pton(AF_INET, config->server_ip, &addr4.sin_addr) != 1) {
                ESP_LOGE(TAG, "inet_pton v4 fail");
                close(sock); goto retry;
            }
            ESP_LOGI(TAG, "Connecting to %s:%d ...", config->server_ip, config->server_port);
            if (connect(sock, (struct sockaddr *)&addr4, sizeof(addr4)) != 0) {
                ESP_LOGW(TAG, "connect v4 err (%d)", errno);
                close(sock); goto retry;
            }
        }

        /* 4. 连接成功 — 记录句柄并走收发任务 */
        xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
        g_sock = sock;
        xSemaphoreGive(g_sock_mutex);

        ESP_LOGI(TAG, "TCP client connected 🎉");
        sock_to_uart_task((void *)(intptr_t)sock);   // 阻塞，直到断线

    retry:
        vTaskDelay(pdMS_TO_TICKS(TCP_RECONNECT_MS));
    }
}

static void sock_to_uart_task(void *arg)
{
    int sock = (intptr_t)arg;
    uint8_t *buf = malloc(TCP_RECV_BUF_SIZE);

    for (;;) {
        int len = recv(sock, buf, TCP_RECV_BUF_SIZE, 0);
        if (len > 0) {
            if (uart_data_cb) {
                uart_data_cb((const uint8_t*)buf, (size_t)len);
            }
        } else {
            break;      // 0 or error → disconnect
        }
    }
    ESP_LOGI(TAG, "Socket closed");

    close(sock);
    xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
    if (g_sock == sock) g_sock = -1;
    xSemaphoreGive(g_sock_mutex);

    free(buf);
    vTaskDelete(NULL);
}