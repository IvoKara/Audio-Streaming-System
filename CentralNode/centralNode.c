#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <netdb.h> 

#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#include <sys/time.h>

#include "MQTTClient.h"

#define CODEC_SAMPLING_RATE (32000)
#define BITS_PER_CHANNEL (16)
#define AUDIO_CHANNELS_CNT (2)

#define BYTES_PER_CHANNEL (BITS_PER_CHANNEL / 8)

#define BYTES_PER_SAMPLE (BYTES_PER_CHANNEL * AUDIO_CHANNELS_CNT)

#define UDP_PORTION_OF_SAMPLES_CNT (256)
#define UDP_PORTION_OF_SAMPLES_BYTES (BYTES_PER_SAMPLE * UDP_PORTION_OF_SAMPLES_CNT)

#define CODEC_PORTION_OF_SAMPLES_CNT (8 * UDP_PORTION_OF_SAMPLES_CNT)
#define CODEC_PORTION_OF_SAMPLES_BYTES (BYTES_PER_SAMPLE * CODEC_PORTION_OF_SAMPLES_CNT)

#define MQTT_BROKER "tcp://192.168.100.171:1883"

struct mbus_cfg_t
{
	char* my_ip_addr;
	unsigned short my_listening_port;
	char* sendto_ip_addr;
	unsigned short sendto_port;
} mbus_cfg = {       
    //.my_ip_addr = "192.168.100.171",
    .my_listening_port = 27772, 
    //.sendto_ip_addr = "192.168.100.128",
    .sendto_port = 37773,
};

static int global_sock;

static MQTTClient client;

static int buffer_frames = CODEC_PORTION_OF_SAMPLES_CNT;
static unsigned int rate = CODEC_SAMPLING_RATE;

static snd_pcm_t* capture_handle;

static pthread_t to_periph_tid;

static void get_my_ip()
{
    char hostname[256];  
	struct hostent *host_info; 
    char *my_ip_addr;

	/* To retrieve hostname */
	gethostname(hostname, sizeof(hostname)); 

	/* To get host information by his name */
	host_info = gethostbyname(hostname); 

	/* To convert an IP from network byte order into ASCII string */
	mbus_cfg.my_ip_addr = inet_ntoa(*((struct in_addr*) host_info->h_addr_list[0])); 
}

static int media_bus_init()
{
	int temp = 0;
	int reuse = 1;

	struct sockaddr_in myAddr;

	struct timeval tv;
	tv.tv_sec = 3;  /* 3 Seconds Time-out */
	tv.tv_usec = 0;

	global_sock = 0;
    /* Get current IP*/
    get_my_ip();
    printf("* host IP: %s\n", mbus_cfg.my_ip_addr); 

	/* Create network socket */
	temp = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (temp < 0) 
	{
		perror("creation socket error");
		return 0;
	}
	global_sock = temp;
	printf("* file descriptor (socket) %d successfully created\n", global_sock);

	/* Set socket options */
	// Specify the receiving timeouts until reporting an error
	temp = setsockopt (global_sock, SOL_SOCKET, SO_RCVTIMEO, (char*) &tv, sizeof(struct timeval));
	if (temp < 0)
	{
		perror("setsockopt SO_RCVTIMEO error");
		return 0;
	}
	printf("* setsockopt SO_RCVTIMEO success\n");

	// Specify that address and port can be reused
	temp = setsockopt (global_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	if (temp < 0) 
	{
		perror("setsockopt SO_REUSEADDR err");
		return 0;
	}
	printf("* setsockopt SO_REUSEADDR success\n");

	/* Binding my (source) ip address and port to the created socket */
	memset(&myAddr, 0, sizeof(struct sockaddr_in));
	myAddr.sin_family = AF_INET;
	myAddr.sin_port = htons(mbus_cfg.my_listening_port);
	inet_aton(mbus_cfg.my_ip_addr, &myAddr.sin_addr);

	temp = bind (global_sock, (struct sockaddr*)&myAddr, sizeof(myAddr));
	if (temp < 0)
	{
		printf("could not bind or connect to socket, error = %d\n", temp);
		perror("");
		return 0;
	}

	printf("* listening on port %d, socket %u\n", mbus_cfg.my_listening_port, global_sock);

    return 1;
}

static int media_bus_send(const void* data_to_send, size_t data_to_send_len)
{
    struct sockaddr_in toAddr;
    int temp = 0;
    
    /* Destination socket */
    toAddr.sin_family = AF_INET;
    
    toAddr.sin_port = htons(mbus_cfg.sendto_port);
    
    inet_aton(mbus_cfg.sendto_ip_addr, &toAddr.sin_addr);

    //printf("sending audio stream to %s\n", mbus_cfg.sendto_ip_addr);

    /* UDP send to destination socket address*/              /*flag*/
    temp = sendto (global_sock, data_to_send, data_to_send_len, 0, (struct sockaddr*)&toAddr, sizeof(toAddr));
    
    if (temp < 0)
    {
        perror("send failed reason");
    }

    return temp;
}

void* stream_to_periph_node_thread(void *para)
{
	char bufferTemp[CODEC_PORTION_OF_SAMPLES_BYTES];  //~8KB
	int err;

    while(1) 
    {
        /* Listening audio capture device for incoming stream - interleaved frames */
        if ((err = snd_pcm_readi (capture_handle, bufferTemp, buffer_frames)) != buffer_frames) 
        {
            printf ("ERROR: read from audio interface failed (%s)\n", snd_strerror (err));
        }

        /* Splitting what was read from capture device (buffer) in small chunks and sending them */
        for (unsigned int i = 0; i < (sizeof(bufferTemp) / (UDP_PORTION_OF_SAMPLES_BYTES)); i++) 
        {
            media_bus_send ((bufferTemp + i * UDP_PORTION_OF_SAMPLES_BYTES), UDP_PORTION_OF_SAMPLES_BYTES);
        }
    }
}

static int audio_interface_init(char *hw_pcm_name)
{
    int err;
    snd_pcm_hw_params_t* hw_params;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;

	/* Open audio capture device */
    if ((err = snd_pcm_open (&capture_handle, hw_pcm_name, SND_PCM_STREAM_CAPTURE, 0)) < 0)
    {
        printf ("ERROR: cannot open audio capture device %s (%s)\n",
                 hw_pcm_name,
                 snd_strerror (err));
        return 0;
    }

    printf("* audio CAPTURE interface opened\n");

	/* Setting blocking mode - block until space is available in the buffer */
    if ((err = snd_pcm_nonblock (capture_handle, 0)) < 0) 
    {
        printf ("ERROR: cannot set block mod (%s)\n",
                snd_strerror (err));
        return 0;
    }

    printf("* set block mode\n");

	/* Allocate space for hardware parameters */
    if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) 
    {
        printf ("ERROR: cannot allocate hardware parameter structure (%s)\n",
                 snd_strerror (err));
        return 0;
    }

    printf("* hw_params allocated\n");

	/* Fill params with a full configuration space for a PCM */
    if ((err = snd_pcm_hw_params_any (capture_handle, hw_params)) < 0) 
    {
        printf ("ERROR: cannot initialize hardware parameter structure (%s)\n",
                 snd_strerror (err));
        return 0;
    }

    printf("* hw_params initialized\n");

	/* Config access mode for hardware parameters - r/w method */
    if ((err = snd_pcm_hw_params_set_access (capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) 
    {
        printf ("ERROR: cannot set access type (%s)\n",
                 snd_strerror (err));
        return 0;
    }

    printf("* hw_params access setted\n");

	/* Config PCM sample format */
    if ((err = snd_pcm_hw_params_set_format (capture_handle, hw_params, format)) < 0) 
    {
        printf ("ERROR: cannot set sample format (%s)\n",
                 snd_strerror (err));
        return 0;
    }

    printf("* hw_params format setted\n");

    /* Config sampling rate near to the target */
    if ((err = snd_pcm_hw_params_set_rate_near (capture_handle, hw_params, &rate, 0)) < 0) 
    {
        printf ("ERROR: cannot set sample rate (%s)\n",
                 snd_strerror (err));
        return 0;
    }

    printf("* hw_params rate setted\n");

    /* Config to use 2 channels - stereo */
    if ((err = snd_pcm_hw_params_set_channels (capture_handle, hw_params, 2)) < 0) 
    {
        printf ("ERROR: cannot set channel count (%s)\n",
                 snd_strerror (err));
        return 0;
    }

    printf("* hw_params channels setted\n");

    /* Set all above configs */
    if ((err = snd_pcm_hw_params (capture_handle, hw_params)) < 0) 
    {
        printf ("ERROR: cannot set parameters (%s)\n",
                 snd_strerror (err));
        return 0;
    }

    printf("* hw_params setted\n");

    /* Remove PCM hardware configuration and free associated resources/memory */
    snd_pcm_hw_params_free (hw_params);

    printf("* hw_params freed\n");

    /* Prepare audio interface for use */
    if ((err = snd_pcm_prepare (capture_handle)) < 0) 
    {
        printf ("ERROR: cannot prepare audio interface for use (%s)\n",
                 snd_strerror (err));
        return 0;
    }

    printf("* audio interface prepared\n");
    return 1;
}

static int mqtt_message_arrive(void *context, char *topicName, int topicLen, MQTTClient_message *message)
{
    /* Copy incoming MQTT payload to global variable */
    mbus_cfg.sendto_ip_addr = (char *)message->payload;
    mbus_cfg.sendto_ip_addr[message->payloadlen] = '\0';
    
    /* Debug */
    printf("%s\n", mbus_cfg.sendto_ip_addr);

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

static int mqtt_client_init(char *topic, int qos)
{
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    int err;
    char *client_id = "raspberry-pi4";

    /* Creation of MQTT client */
    MQTTClient_create(&client, MQTT_BROKER, client_id, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1; //true - information is not saved for previous sessions

    printf("* mqtt client with id '%s' created\n", client_id);

    MQTTClient_setCallbacks(client, NULL, NULL, mqtt_message_arrive, NULL);

    /* Connecting to broker */
    if ((err = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS)
    {
        printf("Failed to connect, return code %d\n", err);
        return 0;
    }

    printf("* mqtt client connected to broker: %s\n", MQTT_BROKER);

    /* Subscribing to given topic */
    MQTTClient_subscribe(client, topic, qos);

    printf("* mqtt client subscribed to topic: %s\n", topic);

    printf("* listening for mqtt message:");

    while(1)
    {
        if (mbus_cfg.sendto_ip_addr != NULL)
        {
            break;
        }
    }

    return 1;
}

static void mqtt_client_deinit(char *topic)
{
    MQTTClient_unsubscribe(client, topic);
    MQTTClient_disconnect(client, 10000);
    MQTTClient_destroy(&client);
}

int main()
{
    char *topic = "/periph/node1/ip";

    /* aloop module capture device */
    char *hw_pcm = "plughw:1,0";

    printf("--------------------------------------\n");

    printf("PREPARE SOCKET:\n");
	media_bus_init();

    printf("--------------------------------------\n");

    printf("PREPARE MQTT COMMUNICATION:\n");
    mqtt_client_init(topic, 1);

    printf("--------------------------------------\n");

    printf("PREPARE AUDIO INTERFACE:\n");
    audio_interface_init(hw_pcm);

    printf("--------------------------------------\n");

    /* Start the thread */
    pthread_create(&to_periph_tid, NULL, stream_to_periph_node_thread, NULL);

    /* wait until is finished */
    pthread_join(to_periph_tid, NULL);
    
    /* Unsubscribe, Delete client */
    mqtt_client_deinit(topic);

    return 0;
}