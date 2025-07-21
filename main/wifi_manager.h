#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"

/* WiFi事件位 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* WiFi管理器初始化和控制函数 */
void wifi_manager_init(void);
void wifi_start_softap(void);
void wifi_start_sta_client(void);
void wifi_start_combined_mode(void);
EventGroupHandle_t wifi_get_event_group(void);

#endif // WIFI_MANAGER_H