#include "webserver.h"
#include "config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WEBSERVER";
static httpd_handle_t server = NULL;
static wifi_stats_t current_stats = {0};

/* NVS存储命名空间 */
#define NVS_NAMESPACE "config"

/* 前向声明 */
static esp_err_t init_nvs_config(void);

/* HTML模板 */
static const char* html_template = 
"<!DOCTYPE html>\n"
"<html><head>\n"
"<title>ESP32 UART-TCP Bridge Config</title>\n"
"<meta charset='UTF-8'>\n"
"<style>\n"
"body{font-family:Arial;margin:20px;background:#f5f5f5}\n"
".container{max-width:600px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1)}\n"
"h1{color:#333;text-align:center}\n"
".section{margin:20px 0;padding:15px;background:#f9f9f9;border-radius:5px}\n"
"label{display:block;margin:10px 0 5px 0;font-weight:bold}\n"
"input,select{width:100%%;padding:8px;margin:5px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}\n"
"button{background:#007cba;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;margin:5px}\n"
"button:hover{background:#005a85}\n"
".stats{background:#e8f4fd;padding:10px;border-left:4px solid #007cba}\n"
".status{padding:5px 10px;border-radius:3px;display:inline-block;margin:5px 0}\n"
".connected{background:#4caf50;color:white}\n"
".disconnected{background:#f44336;color:white}\n"
"</style>\n"
"</head><body>\n"
"<div class='container'>\n"
"<h1>🚀 ESP32 UART-TCP 网桥配置</h1>\n"

"<div class='section'>\n"
"<h3>📊 系统状态</h3>\n"
"<div class='stats'>\n"
"<p><strong>运行时间:</strong> %u 秒</p>\n"
"<p><strong>TCP连接:</strong> <span class='status %s'>%s</span></p>\n"
"<p><strong>WiFi连接:</strong> <span class='status %s'>%s</span></p>\n"
"<p><strong>数据统计:</strong> 发送%u帧 (有效:%u, 无效:%u), 总字节:%u</p>\n"
"</div>\n"
"</div>\n"

"<form method='POST' action='/config'>\n"
"<div class='section'>\n"
"<h3>📶 WiFi STA 配置</h3>\n"
"<label>WiFi名称(SSID):</label>\n"
"<input type='text' name='ssid' value='%s' maxlength='32'>\n"
"<label>WiFi密码:</label>\n"
"<input type='password' name='password' value='%s' maxlength='64'>\n"
"</div>\n"

"<div class='section'>\n"
"<h3>🌐 TCP服务器配置</h3>\n"
"<label>服务器IP:</label>\n"
"<input type='text' name='server_ip' value='%s' maxlength='15'>\n"
"<label>服务器端口:</label>\n"
"<input type='number' name='server_port' value='%d' min='1' max='65535'>\n"
"</div>\n"

"<div class='section'>\n"
"<h3>⚙️ 其他设置</h3>\n"
"<label>开启SoftAP:</label>\n"
"<select name='enable_ap'>\n"
"<option value='1' %s>是 (ESP+MAC后4位)</option>\n"
"<option value='0' %s>否</option>\n"
"</select>\n"
"</div>\n"

"<div class='section'>\n"
"<button type='submit'>💾 保存配置</button>\n"
"<button type='button' onclick='location.href=\"/restart\"'>🔄 重启设备</button>\n"
"<button type='button' onclick='location.reload()'>🔄 刷新状态</button>\n"
"</div>\n"
"</form>\n"

"</div>\n"
"</body></html>";

/* NVS配置操作函数 */
static esp_err_t save_config_to_nvs(const device_config_t* config)
{
    LOG_I(TAG, "准备保存配置到NVS: SSID='%s', Server='%s:%d', AP=%d", 
          config->wifi_ssid, config->server_ip, config->server_port, config->enable_softap);
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        LOG_E(TAG, "无法打开NVS命名空间用于写入: %s", esp_err_to_name(err));
        return err;
    }

    // 逐项保存配置并检查结果
    esp_err_t ssid_err = nvs_set_str(nvs_handle, "wifi_ssid", config->wifi_ssid);
    esp_err_t pass_err = nvs_set_str(nvs_handle, "wifi_pass", config->wifi_password);
    esp_err_t ip_err = nvs_set_str(nvs_handle, "server_ip", config->server_ip);
    esp_err_t port_err = nvs_set_u16(nvs_handle, "server_port", config->server_port);
    esp_err_t ap_err = nvs_set_u8(nvs_handle, "enable_ap", config->enable_softap ? 1 : 0);

    LOG_D(TAG, "NVS写入结果: SSID=%s, PASS=%s, IP=%s, PORT=%s, AP=%s",
          esp_err_to_name(ssid_err), esp_err_to_name(pass_err), 
          esp_err_to_name(ip_err), esp_err_to_name(port_err), esp_err_to_name(ap_err));

    if (ssid_err != ESP_OK || pass_err != ESP_OK || ip_err != ESP_OK || 
        port_err != ESP_OK || ap_err != ESP_OK) {
        LOG_E(TAG, "保存配置到NVS时出现错误");
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    // 提交更改
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        LOG_E(TAG, "提交NVS更改失败: %s", esp_err_to_name(err));
    } else {
        LOG_I(TAG, "✅ 配置已成功保存并提交到NVS");
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t load_config_from_nvs(device_config_t* config)
{
    // 先设置默认配置
    strncpy(config->wifi_ssid, WIFI_SSID, sizeof(config->wifi_ssid) - 1);
    config->wifi_ssid[sizeof(config->wifi_ssid) - 1] = '\0';
    strncpy(config->wifi_password, WIFI_PASS, sizeof(config->wifi_password) - 1);
    config->wifi_password[sizeof(config->wifi_password) - 1] = '\0';
    strncpy(config->server_ip, REMOTE_SERVER_IP, sizeof(config->server_ip) - 1);
    config->server_ip[sizeof(config->server_ip) - 1] = '\0';
    config->server_port = REMOTE_SERVER_PORT;
    config->enable_softap = ENABLE_SOFTAP;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        LOG_I(TAG, "NVS命名空间不存在，使用默认配置");
        return ESP_OK;  // 使用默认配置，不报告为错误
    } else if (err != ESP_OK) {
        LOG_W(TAG, "无法打开NVS命名空间: %s", esp_err_to_name(err));
        return ESP_OK;  // 使用默认配置
    }

    size_t required_size;
    
    // 尝试加载WiFi SSID
    required_size = sizeof(config->wifi_ssid);
    if (nvs_get_str(nvs_handle, "wifi_ssid", config->wifi_ssid, &required_size) == ESP_OK) {
        config->wifi_ssid[required_size-1] = '\0';  // 确保字符串结尾
    }

    // 尝试加载WiFi密码
    required_size = sizeof(config->wifi_password);
    if (nvs_get_str(nvs_handle, "wifi_pass", config->wifi_password, &required_size) == ESP_OK) {
        config->wifi_password[required_size-1] = '\0';
    }

    // 尝试加载服务器IP
    required_size = sizeof(config->server_ip);
    if (nvs_get_str(nvs_handle, "server_ip", config->server_ip, &required_size) == ESP_OK) {
        config->server_ip[required_size-1] = '\0';
    }

    // 尝试加载服务器端口
    nvs_get_u16(nvs_handle, "server_port", &config->server_port);

    // 尝试加载SoftAP设置
    uint8_t enable_ap;
    if (nvs_get_u8(nvs_handle, "enable_ap", &enable_ap) == ESP_OK) {
        config->enable_softap = (enable_ap != 0);
    }

    nvs_close(nvs_handle);
    LOG_I(TAG, "从NVS加载配置完成 - SSID:%s, Server:%s:%d", 
          config->wifi_ssid, config->server_ip, config->server_port);
    return ESP_OK;
}

/* URL解码函数 */
static void url_decode(const char *src, char *dest, size_t dest_size)
{
    size_t i = 0, j = 0;
    while (src[i] && j < dest_size - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            // 解码%xx格式
            int hex_value = 0;
            sscanf(&src[i+1], "%2x", &hex_value);
            dest[j++] = (char)hex_value;
            i += 3;
        } else if (src[i] == '+') {
            // 将+替换为空格
            dest[j++] = ' ';
            i++;
        } else {
            dest[j++] = src[i++];
        }
    }
    dest[j] = '\0';
}

/* HTTP处理函数 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    LOG_I(TAG, "收到配置页面请求");
    
    char* html_content = malloc(4096);
    if (!html_content) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // 从NVS加载当前配置
    device_config_t current_config;
    load_config_from_nvs(&current_config);
    
    // 获取当前状态
    const char* tcp_status_class = current_stats.tcp_connected ? "connected" : "disconnected";
    const char* tcp_status_text = current_stats.tcp_connected ? "已连接" : "未连接";
    const char* sta_status_class = current_stats.sta_connected ? "connected" : "disconnected";  
    const char* sta_status_text = current_stats.sta_connected ? "已连接" : "未连接";
    
    snprintf(html_content, 4096, html_template,
        current_stats.uptime_seconds,
        tcp_status_class, tcp_status_text,
        sta_status_class, sta_status_text,
        current_stats.total_frames_sent,
        current_stats.valid_frames,
        current_stats.invalid_frames,
        current_stats.total_bytes_sent,
        current_config.wifi_ssid,
        current_config.wifi_password,
        current_config.server_ip,
        current_config.server_port,
        current_config.enable_softap ? "selected" : "",
        current_config.enable_softap ? "" : "selected"
    );
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html_content, HTTPD_RESP_USE_STRLEN);
    free(html_content);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    LOG_I(TAG, "收到配置更新请求");
    
    char *buf = malloc(2048);  // 增大到2KB
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    int ret = httpd_req_recv(req, buf, 2047);
    if (ret <= 0) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    LOG_I(TAG, "配置数据: %s", buf);
    
    // 解析表单数据
    device_config_t new_config = {0};
    char *token, *saveptr;
    char *data = strdup(buf);  // 创建副本用于解析
    
    // 初始化默认值
    strncpy(new_config.wifi_ssid, WIFI_SSID, sizeof(new_config.wifi_ssid) - 1);
    strncpy(new_config.wifi_password, WIFI_PASS, sizeof(new_config.wifi_password) - 1);
    strncpy(new_config.server_ip, REMOTE_SERVER_IP, sizeof(new_config.server_ip) - 1);
    new_config.server_port = REMOTE_SERVER_PORT;
    new_config.enable_softap = ENABLE_SOFTAP;
    
    // 解析表单字段
    token = strtok_r(data, "&", &saveptr);
    while (token != NULL) {
        if (strncmp(token, "ssid=", 5) == 0) {
            url_decode(token + 5, new_config.wifi_ssid, sizeof(new_config.wifi_ssid));
        } else if (strncmp(token, "password=", 9) == 0) {
            url_decode(token + 9, new_config.wifi_password, sizeof(new_config.wifi_password));
        } else if (strncmp(token, "server_ip=", 10) == 0) {
            url_decode(token + 10, new_config.server_ip, sizeof(new_config.server_ip));
        } else if (strncmp(token, "server_port=", 12) == 0) {
            new_config.server_port = (uint16_t)atoi(token + 12);
        } else if (strncmp(token, "enable_ap=", 10) == 0) {
            new_config.enable_softap = (atoi(token + 10) != 0);
        }
        token = strtok_r(NULL, "&", &saveptr);
    }
    
    free(data);
    
    // 保存配置到NVS
    esp_err_t save_result = save_config_to_nvs(&new_config);
    
    // 响应页面
    const char* success_html;
    if (save_result == ESP_OK) {
        success_html = 
        "<html><head><meta charset='UTF-8'></head><body><h2>✅ 配置保存成功</h2>"
        "<p>新配置已保存，重启后生效</p>"
        "<p><a href='/'>返回首页</a> | <a href='/restart'>立即重启</a></p>"
        "</body></html>";
        LOG_I(TAG, "配置已成功保存: SSID=%s, IP=%s:%d, AP=%s", 
              new_config.wifi_ssid, new_config.server_ip, new_config.server_port,
              new_config.enable_softap ? "开启" : "关闭");
    } else {
        success_html = 
        "<html><head><meta charset='UTF-8'></head><body><h2>❌ 配置保存失败</h2>"
        "<p>保存配置时出错，请重试</p>"
        "<p><a href='/'>返回首页</a></p>"
        "</body></html>";
    }
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, success_html, HTTPD_RESP_USE_STRLEN);
    
    free(buf);  // 释放内存
    return ESP_OK;
}

static esp_err_t restart_get_handler(httpd_req_t *req)
{
    LOG_W(TAG, "收到重启请求");
    
    const char* restart_html = 
    "<html><head><meta charset='UTF-8'></head><body><h2>设备正在重启...</h2>"
    "<script>setTimeout(function(){window.location.href='/';}, 10000);</script>"
    "<p>请等待约10秒钟后自动跳转</p>"
    "</body></html>";
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, restart_html, HTTPD_RESP_USE_STRLEN);
    
    // 延迟重启，让响应先发送
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

/* URI处理器配置 */
static const httpd_uri_t root_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t config_uri = {
    .uri       = "/config",
    .method    = HTTP_POST,
    .handler   = config_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t restart_uri = {
    .uri       = "/restart",
    .method    = HTTP_GET,
    .handler   = restart_get_handler,
    .user_ctx  = NULL
};

void update_wifi_stats(wifi_stats_t* stats)
{
    if (stats) {
        memcpy(&current_stats, stats, sizeof(wifi_stats_t));
    }
}

esp_err_t get_mac_address_suffix(char* suffix, size_t size)
{
    uint8_t mac[6];
    esp_err_t ret = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (ret == ESP_OK) {
        snprintf(suffix, size, "%02X%02X", mac[4], mac[5]);
    }
    return ret;
}

void start_softap_mode(void)
{
    // 获取MAC地址后4位
    char mac_suffix[8];
    if (get_mac_address_suffix(mac_suffix, sizeof(mac_suffix)) != ESP_OK) {
        strcpy(mac_suffix, "XXXX");
    }
    
    // 创建AP SSID
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s%s", SOFTAP_SSID_PREFIX, mac_suffix);
    
    // 配置AP网络接口
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    
    // 设置AP的IP地址
    esp_netif_ip_info_t ap_ip_info;
    IP4_ADDR(&ap_ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip_info.gw, 192, 168, 4, 1);  
    IP4_ADDR(&ap_ip_info.netmask, 255, 255, 255, 0);
    
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
    
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid_len = strlen(ap_ssid),
            .password = SOFTAP_PASSWORD,
            .max_connection = SOFTAP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .channel = 1,
            .beacon_interval = 100
        },
    };
    strcpy((char*)wifi_ap_config.ap.ssid, ap_ssid);
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    
    LOG_I(TAG, "SoftAP启动: SSID=%s, 密码=%s, IP=192.168.4.1", ap_ssid, SOFTAP_PASSWORD);
}

void init_webserver(void)
{
    if (!ENABLE_SOFTAP) {
        LOG_I(TAG, "SoftAP已禁用，跳过Web服务器初始化");
        return;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.lru_purge_enable = true;
    
    // 增大缓冲区和任务栈大小以处理较长的请求
    config.stack_size = 8192;        // 增大任务栈大小
    config.max_resp_headers = 16;    // 增加响应头数量
    config.recv_wait_timeout = 10;   // 增加接收超时
    config.send_wait_timeout = 10;   // 增加发送超时
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &config_uri);  
        httpd_register_uri_handler(server, &restart_uri);
        LOG_I(TAG, "HTTP服务器启动成功，端口: %d", HTTP_SERVER_PORT);
    } else {
        LOG_E(TAG, "HTTP服务器启动失败");
    }
}

/* NVS初始化函数 */
static esp_err_t init_nvs_config(void)
{
    LOG_I(TAG, "开始初始化NVS配置...");
    
    // 检查NVS分区状态
    nvs_handle_t test_handle;
    esp_err_t open_err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &test_handle);
    LOG_I(TAG, "尝试打开NVS命名空间 '%s': %s", NVS_NAMESPACE, esp_err_to_name(open_err));
    
    if (open_err == ESP_OK) {
        // 命名空间存在，检查是否有数据
        size_t required_size = 0;
        esp_err_t check_err = nvs_get_str(test_handle, "wifi_ssid", NULL, &required_size);
        LOG_I(TAG, "检查现有配置状态: %s", esp_err_to_name(check_err));
        
        if (check_err == ESP_ERR_NVS_NOT_FOUND) {
            // 命名空间存在但没有数据，保存默认配置
            LOG_I(TAG, "命名空间为空，保存默认配置");
            device_config_t default_config;
            load_config_from_nvs(&default_config);  // 获取默认值
            nvs_close(test_handle);
            esp_err_t save_err = save_config_to_nvs(&default_config);
            LOG_I(TAG, "保存默认配置结果: %s", esp_err_to_name(save_err));
            return save_err;
        } else {
            LOG_I(TAG, "发现现有配置，使用已保存的设置");
            nvs_close(test_handle);
            return ESP_OK;
        }
    } else if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        // 命名空间不存在，创建并保存默认配置
        LOG_I(TAG, "创建新的NVS命名空间并保存默认配置");
        device_config_t default_config;
        load_config_from_nvs(&default_config);  // 获取默认值
        esp_err_t save_err = save_config_to_nvs(&default_config);
        LOG_I(TAG, "创建并保存配置结果: %s", esp_err_to_name(save_err));
        return save_err;
    } else {
        LOG_E(TAG, "无法访问NVS: %s", esp_err_to_name(open_err));
        return open_err;
    }
}

/* 对外配置访问接口 */
esp_err_t load_device_config(device_config_t* config)
{
    return load_config_from_nvs(config);
}

/* 设备配置初始化接口 - 在main.c中调用 */
esp_err_t init_device_config(void)
{
    return init_nvs_config();
}