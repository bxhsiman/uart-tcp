// main/main.c
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

#include "config.h"
#include "dataproc.h"
#include "webserver.h"

static const char *TAG = "TCP_UART_WIFI";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num   = 0;

/* Socket 全局句柄保护 */
int g_sock = -1;
SemaphoreHandle_t g_sock_mutex;

/* 全局统计信息 */
static wifi_stats_t g_wifi_stats = {0};
static uint32_t g_start_time = 0;

/* 全局设备配置 */
static device_config_t g_device_config = {0};
static SemaphoreHandle_t g_config_mutex;

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
            LOG_W(TAG, "retry to connect Wi‑Fi");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        LOG_I(TAG,"connect to the AP fail");

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        LOG_I(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        g_wifi_stats.sta_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ----------------- Wi‑Fi STA 初始化 ----------------- */
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 先创建STA接口
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    // 从NVS加载WiFi配置
    device_config_t saved_config;
    esp_err_t config_load_result = load_device_config(&saved_config);
    
    wifi_config_t wifi_config = { 0 };
    if (config_load_result == ESP_OK) {
        strncpy((char*)wifi_config.sta.ssid, saved_config.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char*)wifi_config.sta.password, saved_config.wifi_password, sizeof(wifi_config.sta.password) - 1);
        LOG_I(TAG, "使用保存的WiFi配置: SSID=%s", saved_config.wifi_ssid);
    } else {
        strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
        LOG_W(TAG, "使用默认WiFi配置: SSID=%s", WIFI_SSID);
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(ENABLE_SOFTAP ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    if (ENABLE_SOFTAP) {
        start_softap_mode();
    }
    
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        LOG_I(TAG, "Connected to AP");
    } else {
        LOG_E(TAG, "Failed to connect AP");
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
    LOG_I(TAG, "Socket closed");

    close(sock);
    xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
    if (g_sock == sock) g_sock = -1;
    xSemaphoreGive(g_sock_mutex);

    free(buf);
    vTaskDelete(NULL);
}

/* ======================= Client 模式 ======================= */
static void tcp_client_task(void *arg)
{
    for (;;) {
        // 获取当前配置
        char server_ip[16];
        uint16_t server_port;
        
        xSemaphoreTake(g_config_mutex, portMAX_DELAY);
        strncpy(server_ip, g_device_config.server_ip, sizeof(server_ip) - 1);
        server_ip[sizeof(server_ip) - 1] = '\0';
        server_port = g_device_config.server_port;
        xSemaphoreGive(g_config_mutex);
        
        /* 1. 判断是 IPv4 还是 IPv6 —— 看有没有 ':' 字符 */
        bool is_ipv6 = strchr(server_ip, ':');

        /* 2. 创建相应族的 socket */
        int domain = is_ipv6 ? AF_INET6 : AF_INET;
        int sock   = socket(domain, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) { LOG_E(TAG, "socket() fail (%d)", errno); goto retry; }

        /* 3. 填地址结构并 inet_pton */
        if (is_ipv6) {
            struct sockaddr_in6 addr6 = { 0 };
            addr6.sin6_family = AF_INET6;
            addr6.sin6_port   = htons(server_port);
            if (inet_pton(AF_INET6, server_ip, &addr6.sin6_addr) != 1) {
                LOG_E(TAG, "inet_pton v6 fail");
                close(sock); goto retry;
            }
            LOG_I(TAG, "Connecting to [%s]:%d ...", server_ip, server_port);
            if (connect(sock, (struct sockaddr *)&addr6, sizeof(addr6)) != 0) {
                LOG_W(TAG, "connect v6 err (%d)", errno);
                close(sock); goto retry;
            }
        } else {
            struct sockaddr_in addr4 = { 0 };
            addr4.sin_family = AF_INET;
            addr4.sin_port   = htons(server_port);
            if (inet_pton(AF_INET, server_ip, &addr4.sin_addr) != 1) {
                LOG_E(TAG, "inet_pton v4 fail");
                close(sock); goto retry;
            }
            LOG_I(TAG, "Connecting to %s:%d ...", server_ip, server_port);
            if (connect(sock, (struct sockaddr *)&addr4, sizeof(addr4)) != 0) {
                LOG_W(TAG, "connect v4 err (%d)", errno);
                close(sock); goto retry;
            }
        }

        /* 4. 连接成功 — 记录句柄并走收发任务 */
        xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
        g_sock = sock;
        xSemaphoreGive(g_sock_mutex);

        LOG_I(TAG, "🎉 TCP客户端连接成功! Socket=%d", sock);
        LOG_I(TAG, "🔗 开始双向数据转发...");
        g_wifi_stats.tcp_connected = true;
        sock_to_uart_task((void *)(intptr_t)sock);   // 阻塞，直到断线
        LOG_W(TAG, "🔌 TCP连接断开，准备重连...");
        g_wifi_stats.tcp_connected = false;

    retry:
        vTaskDelay(pdMS_TO_TICKS(TCP_RECONNECT_MS));
    }
}

/* ----------------- 统计更新任务 ----------------- */
static void stats_update_task(void *arg)
{
    for (;;) {
        g_wifi_stats.uptime_seconds = (xTaskGetTickCount() - g_start_time) / configTICK_RATE_HZ;
        
        // 从数据处理模块获取统计信息
        extern uint32_t get_total_frames_sent(void);
        extern uint32_t get_valid_frames(void); 
        extern uint32_t get_invalid_frames(void);
        extern uint32_t get_total_bytes_sent(void);
        
        g_wifi_stats.total_frames_sent = get_total_frames_sent();
        g_wifi_stats.valid_frames = get_valid_frames();
        g_wifi_stats.invalid_frames = get_invalid_frames(); 
        g_wifi_stats.total_bytes_sent = get_total_bytes_sent();
        
        // 更新webserver统计信息
        update_wifi_stats(&g_wifi_stats);
        
        vTaskDelay(pdMS_TO_TICKS(5000)); // 每5秒更新一次
    }
}

/* ----------------- 应用入口 ----------------- */
void app_main(void)
{
    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    
    /* 1.1. 初始化设备配置 */
    LOG_I(TAG, "初始化设备配置...");
    esp_err_t config_ret = init_device_config();
    if (config_ret != ESP_OK) {
        LOG_W(TAG, "设备配置初始化失败: %s", esp_err_to_name(config_ret));
    }

    /* 2. Wi‑Fi */
    g_start_time = xTaskGetTickCount();
    wifi_init_sta();
    
    /* 2.1. Web服务器 */
    if (ENABLE_SOFTAP) {
        init_webserver();
    }

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

    /* 4. 数据处理初始化 */
    init_data_processing();
    g_sock_mutex = xSemaphoreCreateMutex();
    g_config_mutex = xSemaphoreCreateMutex();
    
    /* 4.1. 加载全局配置 */
    if (load_device_config(&g_device_config) == ESP_OK) {
        LOG_I(TAG, "全局配置已加载: SSID=%s, Server=%s:%d", 
              g_device_config.wifi_ssid, g_device_config.server_ip, g_device_config.server_port);
    }

    /* 5. 创建任务 */
    xTaskCreatePinnedToCore(uart_to_sock_task, "uart2sock", 4096,
                            NULL, 12, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(tcp_client_task, "tcp_client", 4096,
                            NULL, 11, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(stats_update_task, "stats", 2048,
                            NULL, 5, NULL, tskNO_AFFINITY);

    LOG_I(TAG, "UART↔TCP Client bridge; target %s:%d",
             g_device_config.server_ip, g_device_config.server_port);
}