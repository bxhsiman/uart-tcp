#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web_server.h"
#include "config.h"
#include "tcp_client.h"
#include "lidar_packet.h"

static const char *TAG = "WEB_SERVER";

static esp_err_t config_get_handler(httpd_req_t *req);
static esp_err_t config_save_handler(httpd_req_t *req);
static esp_err_t reboot_handler(httpd_req_t *req);
static esp_err_t status_handler(httpd_req_t *req);
static esp_err_t favicon_handler(httpd_req_t *req);

static const char* config_html = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset='UTF-8'>"
"<title>ESP32 TCP-UART 配置</title>"
"<style>"
"body { font-family: Arial; margin: 40px; background-color: #f0f0f0; }"
".container { max-width: 500px; margin: 0 auto; background-color: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
"h1 { color: #333; text-align: center; }"
".form-group { margin-bottom: 15px; }"
"label { display: block; margin-bottom: 5px; font-weight: bold; }"
"input[type='text'], input[type='password'], input[type='number'] { width: 100%%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }"
"button { width: 100%%; padding: 12px; background-color: #007bff; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; margin-top: 10px; }"
"button:hover { background-color: #0056b3; }"
".reboot-btn { background-color: #dc3545; }"
".reboot-btn:hover { background-color: #c82333; }"
".status { margin-top: 20px; padding: 10px; border-radius: 4px; }"
".info { background-color: #d1ecf1; border: 1px solid #bee5eb; color: #0c5460; }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<h1>ESP32 TCP-UART 桥接配置</h1>"
"<form action='/save' method='post'>"
"<div class='form-group'>"
"<label for='ssid'>WiFi SSID:</label>"
"<input type='text' id='ssid' name='ssid' value='%s' required>"
"</div>"
"<div class='form-group'>"
"<label for='password'>WiFi 密码:</label>"
"<input type='password' id='password' name='password' value='%s' required>"
"</div>"
"<div class='form-group'>"
"<label for='server_ip'>TCP 服务器 IP:</label>"
"<input type='text' id='server_ip' name='server_ip' value='%s' required>"
"</div>"
"<div class='form-group'>"
"<label for='server_port'>TCP 服务器端口:</label>"
"<input type='number' id='server_port' name='server_port' value='%d' min='1' max='65535' required>"
"</div>"
"<div class='form-group'>"
"<label for='uart_baud'>UART 波特率:</label>"
"<input type='number' id='uart_baud' name='uart_baud' value='%d' required>"
"</div>"
"<div class='form-group'>"
"<label>"
"<input type='checkbox' id='softap_enabled' name='softap_enabled' %s> 启用SoftAP配置模式"
"</label>"
"<small style='color: #666; font-size: 12px;'>关闭后仅保留TCP客户端功能，需重启生效</small>"
"</div>"
"<button type='submit'>保存配置</button>"
"</form>"
"<form action='/reboot' method='post'>"
"<button type='submit' class='reboot-btn'>重启设备</button>"
"</form>"
"<div class='status info'>"
"<strong>当前状态:</strong><br>"
"IP地址: <span id='ip'>等待获取...</span><br>"
"TCP连接: <span id='tcp'>%s</span><br>"
"UART缓冲区: %d bytes<br>"
"<br><strong>UART数据包监控:</strong><br>"
"接收字节: <span id='uart_bytes'>-</span><br>"
"检测包数: <span id='uart_packets'>-</span><br>"
"丢失包数: <span id='uart_lost'>-</span><br>"
"序号错误: <span id='uart_errors'>-</span>"
"</div>"
"</div>"
"<script>"
"function updateStatus() {"
"fetch('/status').then(r=>r.json()).then(d=>{"
"document.getElementById('ip').textContent=d.ip;"
"document.getElementById('tcp').textContent=d.tcp_status;"
"document.getElementById('uart_bytes').textContent=d.uart_bytes;"
"document.getElementById('uart_packets').textContent=d.uart_packets;"
"document.getElementById('uart_lost').textContent=d.uart_lost;"
"document.getElementById('uart_errors').textContent=d.uart_errors;"
"}).catch(e=>console.log('Status update failed:', e));"
"}"
"updateStatus();"
"setInterval(updateStatus, 2000);"  // 每2秒更新一次
"</script>"
"</body>"
"</html>";

httpd_handle_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;   // 增加URI处理器数量
    config.max_resp_headers = 8;    // 增加响应头数量
    config.recv_wait_timeout = 10;  // 接收超时时间(秒)
    config.send_wait_timeout = 10;  // 发送超时时间(秒)
    httpd_handle_t server = NULL;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t config_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = config_get_handler
        };
        httpd_register_uri_handler(server, &config_uri);
        
        httpd_uri_t save_uri = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = config_save_handler
        };
        httpd_register_uri_handler(server, &save_uri);
        
        httpd_uri_t reboot_uri = {
            .uri = "/reboot",
            .method = HTTP_POST,
            .handler = reboot_handler
        };
        httpd_register_uri_handler(server, &reboot_uri);
        
        httpd_uri_t status_uri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_handler
        };
        httpd_register_uri_handler(server, &status_uri);
        
        httpd_uri_t favicon_uri = {
            .uri = "/favicon.ico",
            .method = HTTP_GET,
            .handler = favicon_handler
        };
        httpd_register_uri_handler(server, &favicon_uri);
        
        ESP_LOGI(TAG, "Web server started");
    }
    return server;
}

void web_server_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
        ESP_LOGI(TAG, "Web server stopped");
    }
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    config_t *config = config_get();
    char *resp_str = malloc(4096);
    if (!resp_str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    snprintf(resp_str, 4096, config_html,
             config->wifi_ssid,
             config->wifi_pass,
             config->server_ip,
             config->server_port,
             config->uart_baudrate,
             config->softap_enabled ? "checked" : "",
             tcp_client_is_connected() ? "已连接" : "未连接",
             TCP_RECV_BUF_SIZE);
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    free(resp_str);
    return ESP_OK;
}

static esp_err_t config_save_handler(httpd_req_t *req)
{
    config_t *config = config_get();
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    // 解析POST数据
    char *ssid = strstr(buf, "ssid=");
    char *password = strstr(buf, "password=");
    char *server_ip = strstr(buf, "server_ip=");
    char *server_port = strstr(buf, "server_port=");
    char *uart_baud = strstr(buf, "uart_baud=");
    char *softap_enabled = strstr(buf, "softap_enabled=");
    
    if (ssid && password && server_ip && server_port && uart_baud) {
        // URL解码和提取参数
        ssid += 5;
        char *ssid_end = strchr(ssid, '&');
        if (ssid_end) *ssid_end = '\0';
        
        password += 9;
        char *pass_end = strchr(password, '&');
        if (pass_end) *pass_end = '\0';
        
        server_ip += 10;
        char *ip_end = strchr(server_ip, '&');
        if (ip_end) *ip_end = '\0';
        
        server_port += 12;
        char *port_end = strchr(server_port, '&');
        if (port_end) *port_end = '\0';
        
        uart_baud += 10;
        char *baud_end = strchr(uart_baud, '&');
        if (baud_end) *baud_end = '\0';
        
        // 保存配置
        strncpy(config->wifi_ssid, ssid, sizeof(config->wifi_ssid) - 1);
        strncpy(config->wifi_pass, password, sizeof(config->wifi_pass) - 1);
        strncpy(config->server_ip, server_ip, sizeof(config->server_ip) - 1);
        config->server_port = atoi(server_port);
        config->uart_baudrate = atoi(uart_baud);
        config->softap_enabled = (softap_enabled != NULL);
        
        config_save();
        
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<h1>配置已保存！</h1><p>请重启设备以应用新配置。</p><a href='/'>返回</a>", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_500(req);
    }
    
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h1>设备正在重启...</h1>", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    esp_netif_ip_info_t ip_info = {0};
    char ip_str[16] = "未获取";
    
    // 优先显示SoftAP IP，然后是STA IP
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", IP2STR(&ip_info.ip));
    } else {
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", IP2STR(&ip_info.ip));
        }
    }
    
    lidar_stats_t *stats = lidar_get_stats();
    char resp[512];
    snprintf(resp, sizeof(resp), 
             "{\"ip\":\"%s\",\"tcp_status\":\"%s\",\"uart_bytes\":%lu,\"uart_packets\":%lu,\"uart_lost\":%lu,\"uart_errors\":%lu}",
             ip_str,
             tcp_client_is_connected() ? "已连接" : "未连接",
             stats->total_bytes_received,
             stats->packets_detected,
             stats->packets_lost,
             stats->sequence_errors);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}