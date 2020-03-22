#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"

#include "lwip/sockets.h"

#include "nvs_flash.h"

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

#define LOCAL_LOOPBACK (1)
#define NET_LOOPBACK   (2)
#define TEST_LOOPBACK  (NET_LOOPBACK) //(NET_LOOPBACK) // (LOCAL_LOOPBACK)

#define JSTREAMER_TASK_PRIORITY        5

#define CODEC_SAMPLING_RATE (44100)
#define BITS_PER_CHANNEL (16)
#define AUDIO_CHANNELS_CNT (2)

#define RECORD_RATE         (CODEC_SAMPLING_RATE)
#define RECORD_CHANNEL      (AUDIO_CHANNELS_CNT)
#define RECORD_BITS         (BITS_PER_CHANNEL)

#define SAVE_FILE_RATE      (CODEC_SAMPLING_RATE) //16000
#define SAVE_FILE_CHANNEL   (AUDIO_CHANNELS_CNT) //1
#define SAVE_FILE_BITS      16 //16

#define PLAYBACK_RATE       (CODEC_SAMPLING_RATE)
#define PLAYBACK_CHANNEL    (AUDIO_CHANNELS_CNT)
#define PLAYBACK_BITS       (BITS_PER_CHANNEL)

#define BYTES_PER_CHANNEL (BITS_PER_CHANNEL / 8)

#define BYTES_PER_SAMPLE (BYTES_PER_CHANNEL * AUDIO_CHANNELS_CNT)

#define UDP_PORTION_OF_SAMPLES_CNT (256)
#define UDP_PORTION_OF_SAMPLES_BYTES (BYTES_PER_SAMPLE * UDP_PORTION_OF_SAMPLES_CNT)

#define CODEC_PORTION_OF_SAMPLES_CNT (8 * UDP_PORTION_OF_SAMPLES_CNT)
#define CODEC_PORTION_OF_SAMPLES_BYTES (BYTES_PER_SAMPLE * CODEC_PORTION_OF_SAMPLES_CNT)

typedef enum {
	INTERTASK_CMD_UNKNOWN,
    INTERTASK_CMD_PUSH,
	INTERTASK_CMD_STOP,
} intertask_task_cmd_t;

typedef struct {
    intertask_task_cmd_t     type;
    uint32_t            *pdata;
    int                 index;
    int                 len;
} intertask_task_msg_t;



/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;


static TaskHandle_t         jstreamer_to_net_task_handle;
static TaskHandle_t         jstreamer_from_net_task_handle;
static xQueueHandle         intertask_que;

struct media_bus_t
{
	struct
	{
		TaskHandle_t* thread_id;
		int running;
	} sender;

	struct
	{
		TaskHandle_t* thread_id;
		int running;
	} receiver;

	int sock;
	struct sockaddr_in addr;
} media_bus;

struct mbus_cfg_t
{
	char* my_ip_addr;
	unsigned short my_listening_port;
	char* sendto_ip_addr;
	unsigned short sendto_port;
} mbus_cfg = {
	.my_ip_addr = "192.168.43.116", 
	.my_listening_port = 37773, 
	.sendto_ip_addr = "192.168.43.33",
	.sendto_port = 27772,
};

static char rcv_buff[CODEC_PORTION_OF_SAMPLES_BYTES];

static char biggerTemp[CODEC_PORTION_OF_SAMPLES_BYTES];




static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
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

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "HUAWEI",
            .password = "12345678",
            .bssid_set = false
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

static void wifi_task(void *pvParameters)
{
    tcpip_adapter_ip_info_t ip;
    memset(&ip, 0, sizeof(tcpip_adapter_ip_info_t));
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    while (1) {
        vTaskDelay(2000 / portTICK_PERIOD_MS);

        if (tcpip_adapter_get_ip_info(ESP_IF_WIFI_STA, &ip) == 0) {
            ESP_LOGI(TAG, "~~~~~~~~~~~");
            ESP_LOGI(TAG, "IP:"IPSTR, IP2STR(&ip.ip));
            ESP_LOGI(TAG, "MASK:"IPSTR, IP2STR(&ip.netmask));
            ESP_LOGI(TAG, "GW:"IPSTR, IP2STR(&ip.gw));
            ESP_LOGI(TAG, "~~~~~~~~~~~");
        }
    }
}



static void intertask_que_send(void *que, intertask_task_cmd_t type, void *data, int index, int len, int dir)
{
    intertask_task_msg_t evt = {0};
    evt.type = type;
    evt.pdata = data;
    evt.index = index;
    evt.len = len;
    if (dir) {
        xQueueSendToFront(que, &evt, 0) ;
    } else {
        xQueueSend(que, &evt, 0);
    }
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

static audio_element_handle_t create_raw_reader()
{
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    return raw_stream_init(&raw_cfg);
}

static audio_element_handle_t create_raw_writer()
{
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    return raw_stream_init(&raw_cfg);
}

static esp_err_t recorder_pipeline_read(void *handle, char *data, int data_size)
{
    raw_stream_read(audio_pipeline_get_el_by_tag((audio_pipeline_handle_t)handle, "raw_reader"), data, data_size);
    return ESP_OK;
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

    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
    
	size = recv(media_bus.sock, buff_to_receive, buff_to_receive_len, 0);

	if (size < 0) {
		if ((size == -EAGAIN)) {
			// do nothing
		} else if (size != -EAGAIN) {
			ESP_LOGE(TAG, "Error on receiving - error = %d\n", size);
			perror("receive failed reason");
		}
	}
}

static int media_bus_init(struct mbus_cfg_t* cfg)
{
	int ret = 0;
	struct sockaddr_in myAddr;
	int reuse = 1;
	struct timeval tv;
	tv.tv_sec = 3;  /* 3 Seconds Time-out */
	tv.tv_usec = 0;

	memset(&media_bus, 0, sizeof(struct media_bus_t));

	ret = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(ret < 0) {
		ESP_LOGI(TAG, "socket err");
		perror("");
		return 0;
	}
	media_bus.sock = ret;

	ESP_LOGI(TAG, "socket %d successfully created\n", media_bus.sock);

	/*set the socket options*/
	ret = setsockopt(media_bus.sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv, sizeof(struct timeval));
	if(ret < 0) {
		ESP_LOGI(TAG, "setsockopt SO_RCVTIMEO err");
		perror("");
		return 0;
	}
	ESP_LOGI(TAG, "setsockopt SO_RCVTIMEO success\n");

	ret = setsockopt(media_bus.sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	if(ret < 0) {
		ESP_LOGI(TAG, "setsockopt SO_REUSEADDR err");
		perror("");
		return 0;
	}
	ESP_LOGI(TAG, "setsockopt SO_REUSEADDR success\n");

	memset(&myAddr, 0, sizeof(struct sockaddr_in));
	myAddr.sin_family      = AF_INET;
    inet_aton(cfg->my_ip_addr, &myAddr.sin_addr);
	myAddr.sin_port        = htons(cfg->my_listening_port);
	myAddr.sin_len = sizeof(struct sockaddr_in);

	ret = bind(media_bus.sock, (struct sockaddr *)&myAddr, sizeof(myAddr));
	if (ret < 0)
	{
		ESP_LOGI(TAG, "could not bind or connect to socket, error = %d\n", ret);
		perror("");
		return 0;
	}

    struct ip_mreq group;
    group.imr_multiaddr.s_addr = inet_addr("224.0.0.1");
    group.imr_interface.s_addr = inet_addr(cfg->my_ip_addr);

    if(setsockopt(media_bus.sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&group, sizeof(group)) < 0)
    {
        perror("Adding multicast group error");
        close(media_bus.sock);
        exit(1);
    }

    printf("Adding multicast group...OK.\n");

	ESP_LOGI(TAG, "listening on port %d, socket %u\n", cfg->my_listening_port, media_bus.sock);

	return 1;
}


void jstreamer_from_net_task (void *para)
{
    audio_pipeline_handle_t pipeline_play = NULL;
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();

    static intertask_task_msg_t intertask_msg;

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

    while (1) {
#if (TEST_LOOPBACK == NET_LOOPBACK)
        unsigned int i = 0;
        for (i = 0; i < (sizeof(rcv_buff) / (UDP_PORTION_OF_SAMPLES_BYTES)); i++) {
            mbus_receive((rcv_buff + i * UDP_PORTION_OF_SAMPLES_BYTES), UDP_PORTION_OF_SAMPLES_BYTES);
            //ESP_LOGI(TAG, "RECEIVED %d", i);
        }

        recorder_pipeline_write(pipeline_play, rcv_buff, sizeof(rcv_buff));

#elif (TEST_LOOPBACK == LOCAL_LOOPBACK)
        if (xQueueReceive(intertask_que, &intertask_msg, portMAX_DELAY)) {
            if (intertask_msg.type == INTERTASK_CMD_PUSH) {
                //ESP_LOGE(TAG, "Recv Que INTERTASK_CMD_PUSH");
                recorder_pipeline_write(pipeline_play, biggerTemp, sizeof(biggerTemp));
            } else if (intertask_msg.type == INTERTASK_CMD_STOP) {
                ESP_LOGE(TAG, "Recv Que INTERTASK_CMD_STOP");
                break;
            }
        }
#endif
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

    jstreamer_from_net_task_handle = NULL;
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    nvs_flash_init();

    initialise_wifi();
    xTaskCreate(&wifi_task, "wifi_task", 4096, NULL, 5, NULL);

    media_bus_init(&mbus_cfg);

    // Setup audio codec
		audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);


    intertask_que = xQueueCreate(3, sizeof(intertask_task_msg_t));
    configASSERT(intertask_que);

    if (xTaskCreate(jstreamer_from_net_task, "JStreamerFromNetTask", 3 * 1024, intertask_que,
                    JSTREAMER_TASK_PRIORITY, &jstreamer_from_net_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Error create JStreamerFromNetTask");
    }

    ESP_LOGW(TAG, "Exitting main thread");
}

