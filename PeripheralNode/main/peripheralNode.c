#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"

#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"


#include "mqtt_client.h"
#include "tcpip_adapter.h"

#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "board.h"
#include "audio_hal.h"
#include "filter_resample.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "periph_button.h"

static const char *TAG = "PERIPHERAL_NODE";

#define JSTREAMER_TASK_PRIORITY (5)

#define CODEC_SAMPLING_RATE (44100)
#define BITS_PER_CHANNEL (16)
#define AUDIO_CHANNELS_CNT (2)

#define RECORD_RATE         (CODEC_SAMPLING_RATE)
#define RECORD_CHANNEL      (AUDIO_CHANNELS_CNT)
#define RECORD_BITS         (BITS_PER_CHANNEL)

#define SAVE_FILE_RATE      (CODEC_SAMPLING_RATE) 
#define SAVE_FILE_CHANNEL   (AUDIO_CHANNELS_CNT) 
#define SAVE_FILE_BITS      (16) 

#define PLAYBACK_RATE       (CODEC_SAMPLING_RATE)
#define PLAYBACK_CHANNEL    (AUDIO_CHANNELS_CNT)
#define PLAYBACK_BITS       (BITS_PER_CHANNEL)

#define BYTES_PER_CHANNEL (BITS_PER_CHANNEL / 8)

#define BYTES_PER_SAMPLE (BYTES_PER_CHANNEL * AUDIO_CHANNELS_CNT)

#define UDP_PORTION_OF_SAMPLES_CNT (256)
#define UDP_PORTION_OF_SAMPLES_BYTES (BYTES_PER_SAMPLE * UDP_PORTION_OF_SAMPLES_CNT)

#define CODEC_PORTION_OF_SAMPLES_CNT (8 * UDP_PORTION_OF_SAMPLES_CNT)
#define CODEC_PORTION_OF_SAMPLES_BYTES (BYTES_PER_SAMPLE * CODEC_PORTION_OF_SAMPLES_CNT)

#define IP_ADDRESS_MAX_LEN (16)

struct media_bus_node_t
{
	char ip_addr [IP_ADDRESS_MAX_LEN];
	unsigned short port;
	int socketfd;
} peripheral = {
	.port = 37773
};

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

static char my_ipv4_addr[16];
static int recieved_ip = 0;

static TaskHandle_t stream_from_central_node_thread_handle;
static char rcv_buff[CODEC_PORTION_OF_SAMPLES_BYTES];

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

static int media_bus_init(struct media_bus_node_t *node)
{
	int err = 0;
	int sock = 0;
	int reuse = 1;

	struct sockaddr_in myAddr;

	struct timeval tv;
	tv.tv_sec = 3;  /* 3 Seconds Time-out */
	tv.tv_usec = 0;


	/* Create network socket */
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock <= 0)
	{
		perror("creation socket error");
		return 0;
	}


	/* Set socket options */
	/* 1) Specify the receiving timeouts until reporting an error */
	err = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*) &tv, sizeof(struct timeval));
	if (err < 0)
	{
		printf("setsockopt SO_RCVTIMEO error = %d\n", err);
		perror("");
		return 0;
	}


	/* 2) Specify that address and port can be reused */
	err = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	if (err < 0)
	{
		printf("setsockopt SO_REUSEADDR error = %d\n", err);
		perror("");
		return 0;
	}

    strcpy(peripheral.ip_addr, my_ipv4_addr);
    
	/* Binding my (source) ip address and port to the created socket */
	memset(&myAddr, 0, sizeof(struct sockaddr_in));
	myAddr.sin_family = AF_INET;
	myAddr.sin_port = htons(peripheral.port);
	inet_aton(peripheral.ip_addr, &myAddr.sin_addr);

	err = bind(sock, (struct sockaddr*)&myAddr, sizeof(myAddr));
	if (err < 0)
	{
		printf("could not bind or connect to socket error = %d\n", err);
		perror("");
		return 0;
	}


	/* Set required params for periph node */
	node->socketfd = sock;


	return 1;
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



static audio_element_handle_t create_raw_writer()
{
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    return raw_stream_init(&raw_cfg);
}

static audio_element_handle_t create_filter(int source_rate, int source_channel, int dest_rate, int dest_channel, audio_codec_type_t type)
{
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = source_rate;
    rsp_cfg.src_ch = source_channel;
    rsp_cfg.dest_rate = dest_rate;
    rsp_cfg.dest_ch = dest_channel;
    rsp_cfg.type = type;
    return rsp_filter_init(&rsp_cfg);
}

static audio_element_handle_t create_i2s_stream(int sample_rates, int bits, int channels, audio_stream_type_t type)
{
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = type;
    audio_element_handle_t i2s_stream = i2s_stream_init(&i2s_cfg);
    mem_assert(i2s_stream);
    audio_element_info_t i2s_info = {0};
    audio_element_getinfo(i2s_stream, &i2s_info);
    i2s_info.bits = bits;
    i2s_info.channels = channels;
    i2s_info.sample_rates = sample_rates;
    audio_element_setinfo(i2s_stream, &i2s_info);
    return i2s_stream;
}

static esp_err_t recorder_pipeline_write(void *handle, char *data, int data_size)
{
    raw_stream_write(audio_pipeline_get_el_by_tag((audio_pipeline_handle_t)handle, "raw_writer"), data, data_size);
    return ESP_OK;
}

static void mbus_receive(void * buff_to_receive, size_t buff_to_receive_len)
{
    int i = 0;
    struct sockaddr_in fromAddr;
	int size;
    int ret = 0;

    unsigned int fromAddrLen = sizeof(fromAddr);

    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

    memset(&fromAddr, 0, sizeof(struct sockaddr_in));

	size = recvfrom(peripheral.socketfd, buff_to_receive, buff_to_receive_len, 0, (struct sockaddr*)&fromAddr, (socklen_t *)&fromAddrLen);

	if (size < 0) {
		if ((size == -EAGAIN)) {
			// do nothing
		} else if (size != -EAGAIN) {
			ESP_LOGE(TAG, "Error on receiving - error = %d\n", size);
			perror("receive failed reason");
		}
	} 
}

void stream_from_central_node_thread (void *para)
{
    audio_pipeline_handle_t pipeline_play = NULL;
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();

    /**
     * For the Playback:
     * We will read the recorded file, with sameple rates 16000hz, 16-bits, 1 channel,
     * Decode with WAV decoder
     * And using resample-filter to convert to 48000Hz, 16-bits, 2 channel.
     * Then send the audio to I2S
     */
    ESP_LOGI(TAG, "[2.1] Initialize playback pipeline");
    pipeline_play = audio_pipeline_init(&pipeline_cfg);

    ESP_LOGI(TAG, "[2.2] Create audio elements for playback pipeline");
    audio_element_handle_t raw_writer_el = create_raw_writer();
    audio_element_handle_t filter_upsample_el = create_filter(SAVE_FILE_RATE, SAVE_FILE_CHANNEL, PLAYBACK_RATE, PLAYBACK_CHANNEL, AUDIO_CODEC_TYPE_DECODER);
    audio_element_handle_t i2s_writer_el = create_i2s_stream(PLAYBACK_RATE, PLAYBACK_BITS, PLAYBACK_CHANNEL, AUDIO_STREAM_WRITER);

    ESP_LOGI(TAG, "[2.3] Register audio elements to playback pipeline");
    audio_pipeline_register(pipeline_play, raw_writer_el,        "raw_writer");
    audio_pipeline_register(pipeline_play, filter_upsample_el,   "filter_upsample");
    audio_pipeline_register(pipeline_play, i2s_writer_el,        "i2s_writer");

    /**
     * Audio Playback Flow:
     * [network]-->raw_writer_stream-->filter-->i2s_stream-->[codec_chip]
     */
    ESP_LOGI(TAG, "Link audio elements to make playback pipeline ready");
    audio_pipeline_link(pipeline_play, (const char *[]) {"raw_writer", "filter_upsample", "i2s_writer"}, 3);

    ESP_LOGI(TAG, "Setup file path to read the wav audio to play");
    i2s_stream_set_clk(i2s_writer_el, PLAYBACK_RATE, PLAYBACK_BITS, PLAYBACK_CHANNEL);
    audio_pipeline_run(pipeline_play);

    while (1)
    {
        unsigned int i = 0;
        for (i = 0; i < (sizeof(rcv_buff) / (UDP_PORTION_OF_SAMPLES_BYTES)); i++) 
        {
            mbus_receive((rcv_buff + i * UDP_PORTION_OF_SAMPLES_BYTES), UDP_PORTION_OF_SAMPLES_BYTES);
        }

        recorder_pipeline_write(pipeline_play, rcv_buff, sizeof(rcv_buff));
    }

    ESP_LOGI(TAG, "[ 1.5 ] Stop audio_pipeline");
    audio_pipeline_terminate(pipeline_play);

    audio_pipeline_unregister(pipeline_play, raw_writer_el);
    audio_pipeline_unregister(pipeline_play, filter_upsample_el);
    audio_pipeline_unregister(pipeline_play, i2s_writer_el);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline_play);

    /* Release all resources */
    audio_element_deinit(raw_writer_el);
    audio_element_deinit(filter_upsample_el);
    audio_element_deinit(i2s_writer_el);

    stream_from_central_node_thread_handle = NULL;
    vTaskDelete(NULL);
}

void app_main()
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);

    nvs_flash_init();
    wifi_init();
    mqtt_app_start();

    //receive packets on socket
    media_bus_init(&peripheral);

    // Setup audio codec
	audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    if (xTaskCreate(stream_from_central_node_thread, "JStreamerFromNetTask", 3 * 1024, NULL,
                    JSTREAMER_TASK_PRIORITY, &stream_from_central_node_thread_handle) != pdPASS) {
        ESP_LOGE(TAG, "Error create JStreamerFromNetTask");
    }

    ESP_LOGW(TAG, "Exitting main thread");
}
