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
/* Wi-Fi AP 信息（在 menuconfig → Example Configuration 可改） */
#define WIFI_SSID           "Xiaomi_7E5B"
#define WIFI_PASS           "richbeam"
// #define WIFI_SSID           "miwifi"
// #define WIFI_PASS           "12345678"
#define WIFI_MAX_RETRY      5   // 连接失败重试次数

/* UART 参数 */
#define UART_PORT_NUM       UART_NUM_1
#define UART_BAUD_RATE      921600
#define UART_TX_PIN         17
#define UART_RX_PIN         18
#define UART_BUF_SIZE       2048

/* TCP Server 参数 */
#define TCP_SERVER_PORT     3333
#define TCP_RECV_BUF_SIZE   2048
#define MAX_TCP_CLIENTS     1
/* ---------------------------------------------------------- */

static const char *TAG = "TCP_UART_WIFI";

/*====== Wi-Fi 事件同步 ======*/
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

/*====== TCP 全局句柄保护 ======*/
static int g_sock = -1;                    // 当前活动 client socket
static SemaphoreHandle_t g_sock_mutex;     // 互斥保护 g_sock

/* ----------------- Wi-Fi 事件回调 ----------------- */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "retry to connect Wi-Fi");
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

/* ----------------- Wi-Fi Station 初始化 ----------------- */
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
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            /* 其他 STA 字段保持默认 0 */
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* 等待连接成功（或失败） */
    EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PASS);
    }

    /* 可取消事件回调注册，但通常保持注册继续监听即可 */
}

/* ----------------- UART → TCP 任务 ----------------- */
static void uart_to_tcp_task(void *arg)
{
    uint8_t *buf = (uint8_t *)malloc(UART_BUF_SIZE);
    for (;;) {
        int len = uart_read_bytes(UART_PORT_NUM, buf, UART_BUF_SIZE, pdMS_TO_TICKS(100));
        if (len > 0) {
            xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
            int sock = g_sock;
            xSemaphoreGive(g_sock_mutex);

            if (sock >= 0) {
                int sent = send(sock, buf, len, 0);
                if (sent < 0) {
                    ESP_LOGW(TAG, "TCP send error (%d)", errno);
                }
            }
        }
    }
    /* 不会到这里 */
}

/* ----------------- TCP → UART 任务 ----------------- */
static void tcp_to_uart_task(void *arg)
{
    int sock = (intptr_t)arg;
    uint8_t *buf = (uint8_t *)malloc(TCP_RECV_BUF_SIZE);

    for (;;) {
        int len = recv(sock, buf, TCP_RECV_BUF_SIZE, 0);
        if (len > 0) {
            uart_write_bytes(UART_PORT_NUM, (const char *)buf, len);
        } else {
            break;  // 断开
        }
    }
    ESP_LOGI(TAG, "Client disconnected");

    close(sock);
    xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
    if (g_sock == sock) g_sock = -1;
    xSemaphoreGive(g_sock_mutex);

    free(buf);
    vTaskDelete(NULL);
}

/* ----------------- TCP Server 任务 ----------------- */
static void tcp_server_task(void *arg)
{
    struct sockaddr_in6 listen_addr = {
        .sin6_family = AF_INET6,
        .sin6_port   = htons(TCP_SERVER_PORT),
        .sin6_addr   = in6addr_any
    };

    int listen_sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    if (bind(listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) != 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    listen(listen_sock, MAX_TCP_CLIENTS);
    ESP_LOGI(TAG, "TCP server listening on port %d", TCP_SERVER_PORT);

    while (1) {
        struct sockaddr_in6 client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Socket accept failed: errno %d", errno);
            continue;
        }

        /* 如果已有活动 client，拒绝新的连接 */
        xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
        if (g_sock >= 0) {
            ESP_LOGW(TAG, "Already connected, rejecting another client");
            close(sock);
            xSemaphoreGive(g_sock_mutex);
            continue;
        }
        g_sock = sock;
        xSemaphoreGive(g_sock_mutex);

        ESP_LOGI(TAG, "Client connected");

        /* 启动 TCP→UART 方向任务（UART→TCP 方向任务常驻、共享 g_sock） */
        xTaskCreatePinnedToCore(tcp_to_uart_task, "tcp_to_uart", 4096,
                                (void *)(intptr_t)sock, 12, NULL, tskNO_AFFINITY);
    }

    /* 不会到这里 */
}

/* ----------------- 应用入口 ----------------- */
void app_main(void)
{
    /* 1. NVS（Wi-Fi 需要） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. 连接 Wi-Fi AP */
    wifi_init_sta();

    /* 3. UART 驱动初始化 */
    const uart_config_t uart_cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2,
                                        0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* 4. 创建互斥与任务 */
    g_sock_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(uart_to_tcp_task, "uart_to_tcp", 4096,
                            NULL, 12, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(tcp_server_task, "tcp_server", 4096,
                            NULL, 11, NULL, tskNO_AFFINITY);

    ESP_LOGI(TAG, "UART<->TCP bridge ready. Connect via telnet to port %d", TCP_SERVER_PORT);
}
