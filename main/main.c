// main/tcp_uart_wifi_bridge.c
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

/* ------------------------ 项目配置 ------------------------ */
/* Wi‑Fi STA 信息（也可移入 Kconfig）*/
#define WIFI_SSID           "Xiaomi_7E5B"
#define WIFI_PASS           "richbeam"
#define WIFI_MAX_RETRY      5

/* UART 参数 */
#define UART_PORT_NUM       UART_NUM_1
#define UART_BAUD_RATE      921600
#define UART_TX_PIN         17
#define UART_RX_PIN         18
#define UART_BUF_SIZE       2048

/* ========= TCP 角色选择 ========= */
#define TCP_BRIDGE_CLIENT   1       // ★ 0 = 当 Server；1 = 当 Client
/* -------------------------------- */

/* ■ Server 模式专用宏 */
#define TCP_SERVER_PORT     3333
#define MAX_TCP_CLIENTS     1

/* ■ Client 模式专用宏 */
#define REMOTE_SERVER_IP    "192.168.114.117"   // ★ 改成你的服务器 IP
#define REMOTE_SERVER_PORT  3334            // ★ 改成你的服务器端口
#define TCP_RECONNECT_MS    5000            // ★ 断线后重新连接间隔

/* 公共缓冲区 */
#define TCP_RECV_BUF_SIZE   2048
/* ---------------------------------------------------------- */

static const char *TAG = "TCP_UART_WIFI";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num   = 0;

/* ===== Socket 全局句柄保护 ===== */
static int g_sock = -1;
static SemaphoreHandle_t g_sock_mutex;

/* ----------------- Wi‑Fi 事件回调 ----------------- */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "retry to connect Wi‑Fi");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ----------------- Wi‑Fi STA 初始化 ----------------- */
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = { .sta = {
        .ssid = WIFI_SSID,
        .password = WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP");
    } else {
        ESP_LOGE(TAG, "Failed to connect AP");
    }
}

/* ----------------- UART → Socket 任务 ----------------- */
static void uart_to_sock_task(void *arg)
{
    uint8_t *buf = malloc(UART_BUF_SIZE);
    for (;;) {
        int len = uart_read_bytes(UART_PORT_NUM, buf, UART_BUF_SIZE, pdMS_TO_TICKS(100));
        if (len > 0) {
            xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
            int sock = g_sock;
            xSemaphoreGive(g_sock_mutex);

            if (sock >= 0) {
                int off = 0;
                while (off < len) {
                    int sent = send(sock, buf + off, len - off, 0);
                    if (sent <= 0) break;
                    off += sent;
                }
            }
        }
    }
}

/* ----------------- Socket → UART 任务 ----------------- */
static void sock_to_uart_task(void *arg)
{
    int sock = (intptr_t)arg;
    uint8_t *buf = malloc(TCP_RECV_BUF_SIZE);

    for (;;) {
        int len = recv(sock, buf, TCP_RECV_BUF_SIZE, 0);
        if (len > 0) {
            uart_write_bytes(UART_PORT_NUM, (const char *)buf, len);
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

/* ======================= Server 模式 ======================= */
#if !TCP_BRIDGE_CLIENT
static void tcp_server_task(void *arg)
{
    struct sockaddr_in6 listen_addr = {
        .sin6_family = AF_INET6,
        .sin6_port   = htons(TCP_SERVER_PORT),
        .sin6_addr   = in6addr_any
    };

    int listen_sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) { ESP_LOGE(TAG, "socket err"); vTaskDelete(NULL); }

    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    ESP_ERROR_CHECK(bind(listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)));
    listen(listen_sock, MAX_TCP_CLIENTS);
    ESP_LOGI(TAG, "Listening on port %d", TCP_SERVER_PORT);

    while (1) {
        struct sockaddr_in6 addr; socklen_t len = sizeof(addr);
        int sock = accept(listen_sock, (struct sockaddr *)&addr, &len);
        if (sock < 0) continue;

        xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
        if (g_sock >= 0) {             // 已有连接
            ESP_LOGW(TAG, "Busy; reject new client");
            close(sock);
            xSemaphoreGive(g_sock_mutex);
            continue;
        }
        g_sock = sock;
        xSemaphoreGive(g_sock_mutex);

        ESP_LOGI(TAG, "Client connected");
        xTaskCreatePinnedToCore(sock_to_uart_task, "sock2uart", 4096,
                                (void *)(intptr_t)sock, 12, NULL, tskNO_AFFINITY);
    }
}
#endif

/* ======================= Client 模式 ======================= */
#if TCP_BRIDGE_CLIENT
static void tcp_client_task(void *arg)
{
    for (;;) {
        /* 1. 判断是 IPv4 还是 IPv6 —— 看有没有 ':' 字符 */
        bool is_ipv6 = strchr(REMOTE_SERVER_IP, ':');

        /* 2. 创建相应族的 socket */
        int domain = is_ipv6 ? AF_INET6 : AF_INET;
        int sock   = socket(domain, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) { ESP_LOGE(TAG, "socket() fail (%d)", errno); goto retry; }

        /* 3. 填地址结构并 inet_pton */
        if (is_ipv6) {
            struct sockaddr_in6 addr6 = { 0 };
            addr6.sin6_family = AF_INET6;
            addr6.sin6_port   = htons(REMOTE_SERVER_PORT);
            if (inet_pton(AF_INET6, REMOTE_SERVER_IP, &addr6.sin6_addr) != 1) {
                ESP_LOGE(TAG, "inet_pton v6 fail");
                close(sock); goto retry;
            }
            ESP_LOGI(TAG, "Connecting to [%s]:%d ...", REMOTE_SERVER_IP, REMOTE_SERVER_PORT);
            if (connect(sock, (struct sockaddr *)&addr6, sizeof(addr6)) != 0) {
                ESP_LOGW(TAG, "connect v6 err (%d)", errno);
                close(sock); goto retry;
            }
        } else {
            struct sockaddr_in addr4 = { 0 };
            addr4.sin_family = AF_INET;
            addr4.sin_port   = htons(REMOTE_SERVER_PORT);
            if (inet_pton(AF_INET, REMOTE_SERVER_IP, &addr4.sin_addr) != 1) {
                ESP_LOGE(TAG, "inet_pton v4 fail");
                close(sock); goto retry;
            }
            ESP_LOGI(TAG, "Connecting to %s:%d ...", REMOTE_SERVER_IP, REMOTE_SERVER_PORT);
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
#endif

/* ----------------- 应用入口 ----------------- */
void app_main(void)
{
    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. Wi‑Fi */
    wifi_init_sta();

    /* 3. UART */
    const uart_config_t uc = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE*2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uc));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* 4. 互斥 & 任务 */
    g_sock_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(uart_to_sock_task, "uart2sock", 4096,
                            NULL, 12, NULL, tskNO_AFFINITY);

#if TCP_BRIDGE_CLIENT
    xTaskCreatePinnedToCore(tcp_client_task, "tcp_client", 4096,
                            NULL, 11, NULL, tskNO_AFFINITY);
    ESP_LOGI(TAG, "UART↔TCP *Client* bridge; target %s:%d",
             REMOTE_SERVER_IP, REMOTE_SERVER_PORT);
#else
    xTaskCreatePinnedToCore(tcp_server_task, "tcp_server", 4096,
                            NULL, 11, NULL, tskNO_AFFINITY);
    ESP_LOGI(TAG, "UART↔TCP *Server* bridge; listening on %d",
             TCP_SERVER_PORT);
#endif
}
