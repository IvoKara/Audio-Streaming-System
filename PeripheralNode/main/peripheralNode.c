#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "tcpip_adapter.h"

static const char *TAG = "PERIPHERAL_NODE";

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

static char my_ipv4_addr[16];

static int recieved_ip = 0;

static void nbo_to_str_ip(char *network_byte_order)
{
    int nbo_len = 8;
    char octet_hex[5];
	char octet_dec[5];
	char *sep = ".";

	strcpy(my_ipv4_addr, "");
	
	for (int i = nbo_len - 2; i >= 0; i-=2)
	{
		sprintf(octet_hex, "%c%c", network_byte_order[i], network_byte_order[i+1]);
		if(i == 0)
		{
			sep = "";
		}
		sprintf(octet_dec, "%d%s", (int)strtol(octet_hex, NULL, 16), sep);
		strcat(my_ipv4_addr, octet_dec);
	}
}

static void get_my_ip()
{
    /* GET TCP/IP adapter info */
    tcpip_adapter_ip_info_t ipInfo; 
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ipInfo);
    
    /* GLOBAL - Convert IP from Network Byte Oreder to String */
    char ip_network_byte_order[8];
    sprintf(ip_network_byte_order, "%x", ipInfo.ip.addr);
    nbo_to_str_ip(ip_network_byte_order);
}



static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    int qos = 1;

    char rcv_topic[strlen(CONFIG_TOPIC)*2];

    strcpy(rcv_topic, CONFIG_TOPIC);
    strcat(rcv_topic, "/received");

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            
            msg_id = esp_mqtt_client_subscribe(client, CONFIG_TOPIC, qos);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, rcv_topic, qos);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            
            if (strncmp(event->topic, rcv_topic, strlen(rcv_topic)) == 0)
            {
                recieved_ip = 1;
            }

            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
            
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URL,
        .event_handle = mqtt_event_handler,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);

    /* Get device IP address */
    get_my_ip();

    /* portTICK_PERIOD_MS -> to calculate real time from tick rate */
    const TickType_t milli_seconds = 5000 / portTICK_PERIOD_MS; 

    while (!recieved_ip)
    {
        vTaskDelay(milli_seconds);
        esp_mqtt_client_publish(client, CONFIG_TOPIC, my_ipv4_addr, 0, 1, 0);
        ESP_LOGI(TAG, "sent publish successful");   
        vTaskDelay(milli_seconds);
    }
}



static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);

            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void wifi_init(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "start the WIFI SSID:[%s]", CONFIG_WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Waiting for wifi");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}



void app_main()
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);

    nvs_flash_init();
    wifi_init();
    mqtt_app_start();
}
