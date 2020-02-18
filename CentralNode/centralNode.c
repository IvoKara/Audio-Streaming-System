#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#include <sys/time.h>

#include "MQTTClient.h"

#define CODEC_SAMPLING_RATE (44100)
#define BITS_PER_CHANNEL (16)
#define AUDIO_CHANNELS_CNT (2)

#define BYTES_PER_CHANNEL (BITS_PER_CHANNEL / 8)

#define BYTES_PER_SAMPLE (BYTES_PER_CHANNEL * AUDIO_CHANNELS_CNT)

#define UDP_PORTION_OF_SAMPLES_CNT (256)
#define UDP_PORTION_OF_SAMPLES_BYTES (BYTES_PER_SAMPLE * UDP_PORTION_OF_SAMPLES_CNT)

#define CODEC_PORTION_OF_SAMPLES_CNT (8 * UDP_PORTION_OF_SAMPLES_CNT)
#define CODEC_PORTION_OF_SAMPLES_BYTES (BYTES_PER_SAMPLE * CODEC_PORTION_OF_SAMPLES_CNT)

#define MQTT_BROKER_URI "tcp://localhost:1883"
#define PERIPHERAL_NODE_TOPIC_GET_IP "home/+/ip"

#define PERIPHERAL_NODES_MAX_NUMBER (6)

#define IP_ADDRESS_MAX_LEN (15)


static struct media_bus_node_t
{
	char ip_addr [IP_ADDRESS_MAX_LEN];
	unsigned short port;
    int socketfd;
    pthread_t stream_thread_id;

} central = {
    .port = 27772
};

struct media_bus_node_t periphs[PERIPHERAL_NODES_MAX_NUMBER];
static unsigned int periph_ip_index = 0;

static MQTTClient client;

static snd_pcm_t* capture_handle;



static void get_my_ip(char *network_interface)
{
    char *my_ip;
    struct ifreq ifr;
    
    /* Creating socket */
    int socketfd = socket(AF_INET, SOCK_DGRAM, 0);

    /* Accessing network interface information by passing address using ioctl sys call */
    ifr.ifr_addr.sa_family = AF_INET;
	strncpy(ifr.ifr_name , network_interface, IFNAMSIZ - 1);
	ioctl(socketfd, SIOCGIFADDR, &ifr);
    
    /* Closing socket */
    close(socketfd);

    /* Extracting IP address */
    my_ip = inet_ntoa(((struct sockaddr_in *) &ifr.ifr_addr)->sin_addr);

    /* Copying to global variable */
    strcpy(central.ip_addr, my_ip);
    printf("* device IP: %s\n", central.ip_addr);
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


	/* Binding my (source) ip address and port to the created socket */
	memset(&myAddr, 0, sizeof(struct sockaddr_in));
	myAddr.sin_family = AF_INET;
	myAddr.sin_port = htons(central.port);
	inet_aton(central.ip_addr, &myAddr.sin_addr);

	err = bind(sock, (struct sockaddr*)&myAddr, sizeof(myAddr));
	if (err < 0)
	{
		printf("could not bind or connect to socket error = %d\n", err);
		perror("");
		return 0;
	}


	/* Set required params for periph node */
	node->port = 37773;
	node->socketfd = sock;


	return 1;
}

static int media_bus_send(struct media_bus_node_t *node, const void* data_to_send, int data_to_send_len)
{
    int temp = 0;
    struct sockaddr_in toAddr;


    /* Destination socket */
    toAddr.sin_family = AF_INET;
    toAddr.sin_port = htons(node->port);
    inet_aton(node->ip_addr, &toAddr.sin_addr);

//    printf("%d %d %s\n", node->socketfd, node->port, node->ip_addr);

    /* UDP send to destination socket address*/              /*flag*/
    temp = sendto(node->socketfd, data_to_send, data_to_send_len, 0, (struct sockaddr*)&toAddr, sizeof(toAddr));

    if (temp < 0)
    {
        perror("send failed reason");
    }

    return temp;
}

void* stream_to_periph_node_thread(void *node)
{
	int err;
    int buffer_frames = CODEC_PORTION_OF_SAMPLES_CNT;

	char bufferTemp[CODEC_PORTION_OF_SAMPLES_BYTES];  //~8KB

    while (1) 
    {
        /* Listening audio capture device for incoming stream - interleaved frames */
	    err = snd_pcm_readi(capture_handle, bufferTemp, buffer_frames);
        if (err != buffer_frames) 
        {
            printf("ERROR: read from audio interface failed (%s)\n", snd_strerror (err));
        }


        /* Splitting what was read from capture device (buffer) in small chunks and sending them */
        for (unsigned int i = 0; i < (sizeof(bufferTemp) / (UDP_PORTION_OF_SAMPLES_BYTES)); i++) 
        {
            media_bus_send (
                (struct media_bus_node_t *) node,
                (bufferTemp + i * UDP_PORTION_OF_SAMPLES_BYTES),
                UDP_PORTION_OF_SAMPLES_BYTES
            );
        }
    }
}

static int start_streaming_threads()
{
    int temp = 0;

    while (1)
    {
        if (temp < periph_ip_index && periphs[temp].socketfd != 0)
        {
            printf("streaming to %s on socket %d\n", periphs[temp].ip_addr, periphs[temp].socketfd);

            pthread_create(
                &periphs[temp].stream_thread_id, 
                NULL, 
                stream_to_periph_node_thread, 
                ((void *) &periphs[temp])
            );
            //pthread_join(periphs[temp].stream_thread_id, NULL);

            temp++;

            if (temp >= PERIPHERAL_NODES_MAX_NUMBER)
            {
                perror("Too many peripheral nodes");
                return 0;
            }
        }
    }
    
    return 1;
}



static int audio_interface_init(char *hw_pcm_name)
{
    int err;
    unsigned int rate = CODEC_SAMPLING_RATE;
    
    snd_pcm_hw_params_t* hw_params;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;


	/* Open audio capture device */
    err = snd_pcm_open(&capture_handle, hw_pcm_name, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0)
    {
        printf("ERROR: cannot open audio capture device %s (%s)\n",
                 hw_pcm_name,
                 snd_strerror (err));
        return 0;
    }    
    printf("* audio CAPTURE interface opened\n");

	
    /* Setting blocking mode - block until space is available in the buffer */
    err = snd_pcm_nonblock(capture_handle, 0);
    if (err < 0) 
    {
        printf("ERROR: cannot set block mod (%s)\n",
                snd_strerror (err));
        return 0;
    }
    printf("* set block mode\n");


	/* Allocate space for hardware parameters */
    err = snd_pcm_hw_params_malloc(&hw_params);
    if (err < 0) 
    {
        printf("ERROR: cannot allocate hardware parameter structure (%s)\n",
                 snd_strerror (err));
        return 0;
    }
    printf("* hw_params allocated\n");


	/* Fill params with a full configuration space for a PCM */
    err = snd_pcm_hw_params_any(capture_handle, hw_params);
    if (err < 0) 
    {
        printf("ERROR: cannot initialize hardware parameter structure (%s)\n",
                 snd_strerror (err));
        return 0;
    }
    printf("* hw_params initialized\n");


	/* Config access mode for hardware parameters - r/w method */
    err = snd_pcm_hw_params_set_access(capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) 
    {
        printf("ERROR: cannot set access type (%s)\n",
                 snd_strerror (err));
        return 0;
    }
    printf("* hw_params access setted\n");


	/* Config PCM sample format */
    err = snd_pcm_hw_params_set_format(capture_handle, hw_params, format);
    if (err < 0) 
    {
        printf("ERROR: cannot set sample format (%s)\n",
                 snd_strerror (err));
        return 0;
    }
    printf("* hw_params format setted\n");


    /* Config sampling rate near to the target */
    err = snd_pcm_hw_params_set_rate_near(capture_handle, hw_params, &rate, 0);
    if (err < 0) 
    {
        printf("ERROR: cannot set sample rate (%s)\n",
                 snd_strerror (err));
        return 0;
    }
    printf("* hw_params rate setted\n");


    /* Config to use 2 channels - stereo */
    err = snd_pcm_hw_params_set_channels(capture_handle, hw_params, 2);
    if (err < 0) 
    {
        printf("ERROR: cannot set channel count (%s)\n",
                 snd_strerror (err));
        return 0;
    }
    printf("* hw_params channels setted\n");


    /* Set all above configs */
    err = snd_pcm_hw_params(capture_handle, hw_params);
    if (err < 0) 
    {
        printf("ERROR: cannot set parameters (%s)\n",
                 snd_strerror (err));
        return 0;
    }
    printf("* hw_params setted\n");


    /* Remove PCM hardware configuration and free associated resources/memory */
    snd_pcm_hw_params_free(hw_params);
    printf("* hw_params freed\n");


    /* Prepare audio interface for use */
    err = snd_pcm_prepare(capture_handle);
    if (err < 0) 
    {
        printf("ERROR: cannot prepare audio interface for use (%s)\n",
                 snd_strerror (err));
        return 0;
    }
    printf("* audio interface prepared\n");


    return 1;
}



static int mqtt_message_send(char *topic, char *payload)
{
    MQTTClient_message publish_message = MQTTClient_message_initializer;
    publish_message.payload = payload;
    publish_message.payloadlen = (int) strlen(payload);
    publish_message.qos = 1;
    publish_message.retained = 0;

    strcat(topic, "/received");

    /* publish MQTT message to stop receiving IP address from the corresponding node */
    MQTTClient_publishMessage(client, topic, &publish_message, NULL);
}

static int mqtt_message_arrive(void *context, char *topicName, int topicLen, MQTTClient_message *message)
{
    /* Copy incoming MQTT payload to global variable */
    strcpy(periphs[periph_ip_index].ip_addr, message->payload);

    /* Create socket to the corresponding Peripheral Node */
    media_bus_init(&periphs[periph_ip_index++]);

    /* Send 'receive' message */
    mqtt_message_send(topicName, "1");

    /* Clean allocated memory for MQTT message */
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);

    return 1;
}

static int mqtt_client_init(char *topic, int qos)
{
    int err;
    char *client_id = "raspberry-pi4";
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;


    /* Creation of MQTT client */
    MQTTClient_create(&client, MQTT_BROKER_URI, client_id, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1; /* true - information is not saved for previous sessions */
    printf("* mqtt client with id '%s' created\n", client_id);

    
    /* Setting callback function for receiving messages */
    MQTTClient_setCallbacks(client, NULL, NULL, mqtt_message_arrive, NULL);


    /* Connecting to broker */
    err = MQTTClient_connect(client, &conn_opts);
    if (err != MQTTCLIENT_SUCCESS)
    {
        printf("Failed to connect, return code %d\n", err);
        return 0;
    }
    printf("* mqtt client connected to broker: %s\n", MQTT_BROKER_URI);


    /* Subscribing to given topic */
    MQTTClient_subscribe(client, topic, qos);
    printf("* mqtt client subscribed to topic: %s\n", topic);


    /* Wait till receive ONE IP address */
    printf("* waiting to recieve IP...\n");
    while (!periph_ip_index) { }
    printf("* received first IP to stream\n");


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
    /* Network interface - wlan0->wifi; eth0 -> ethernet */
    char *net_int = "wlan0";

    /* Subscription topic to recieve peripheral node ip */
    char *topic = PERIPHERAL_NODE_TOPIC_GET_IP;

    /* aloop module capture device */
    char *hw_pcm = "hw:1,1";

    printf("--------------------------------------\n");

    printf("INTERNAL INFORMATION:\n");
    get_my_ip(net_int);

    printf("--------------------------------------\n");

    printf("INIT MQTT COMMUNICATION:\n");
    mqtt_client_init(topic, 1);

    printf("--------------------------------------\n");

    printf("PREPARE AUDIO INTERFACE:\n");
    audio_interface_init(hw_pcm);

    printf("--------------------------------------\n");

    printf("START STREAMING THREADS:\n");
    start_streaming_threads();


    /* Unsubscribe, Delete client */
    mqtt_client_deinit(topic);

    return 0;
}
