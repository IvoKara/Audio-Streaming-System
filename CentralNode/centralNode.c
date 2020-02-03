#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#define CODEC_SAMPLING_RATE (32000)
#define BITS_PER_CHANNEL (16)
#define AUDIO_CHANNELS_CNT (2)

#define BYTES_PER_CHANNEL (BITS_PER_CHANNEL / 8)

#define BYTES_PER_SAMPLE (BYTES_PER_CHANNEL * AUDIO_CHANNELS_CNT)

#define UDP_PORTION_OF_SAMPLES_CNT (256)
#define UDP_PORTION_OF_SAMPLES_BYTES (BYTES_PER_SAMPLE * UDP_PORTION_OF_SAMPLES_CNT)

#define CODEC_PORTION_OF_SAMPLES_CNT (8 * UDP_PORTION_OF_SAMPLES_CNT)
#define CODEC_PORTION_OF_SAMPLES_BYTES (BYTES_PER_SAMPLE * CODEC_PORTION_OF_SAMPLES_CNT)

struct media_bus_t
{
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
	.my_ip_addr = "192.168.100.171",     /* static ip of RPi on my router */
	.my_listening_port = 27772, 
	.sendto_ip_addr = "192.168.100.128", /* DHCP ip of ESP32 */
	.sendto_port = 37773,
};

static char rcv_buff[UDP_PORTION_OF_SAMPLES_BYTES];

static int buffer_frames = CODEC_PORTION_OF_SAMPLES_CNT;
static unsigned int rate = CODEC_SAMPLING_RATE;

static snd_pcm_t *capture_handle;

static int media_bus_init(struct mbus_cfg_t* cfg)
{
	int temp = 0;
	int reuse = 1;

	struct sockaddr_in myAddr;

	struct timeval tv;
	tv.tv_sec = 3;  /* 3 Seconds Time-out */
	tv.tv_usec = 0;

	/* Initial zero value for media_bus */
	memset(&media_bus, 0, sizeof(struct media_bus_t));

	/* Create network socket */
	temp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(temp < 0) 
	{
		perror("creation socket error");
		goto exit;
	}
	media_bus.sock = temp;
	printf("file descriptor (socket) %d successfully created\n", media_bus.sock);

	/* Set socket options */
	// Specify the receiving timeouts until reporting an error
	temp = setsockopt(media_bus.sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv, sizeof(struct timeval));
	if(temp < 0)
	{
		perror("setsockopt SO_RCVTIMEO error");
		goto exit;
	}
	printf("setsockopt SO_RCVTIMEO success\n");

	// Specify that address and port can be reused
	temp = setsockopt(media_bus.sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	if(temp < 0) 
	{
		perror("setsockopt SO_REUSEADDR err");
		goto exit;
	}
	printf("setsockopt SO_REUSEADDR success\n");

	/* Binding ip address and port to the created socket */
	memset(&myAddr, 0, sizeof(struct sockaddr_in));
	myAddr.sin_family = AF_INET;
	myAddr.sin_port = htons(cfg->my_listening_port);
	inet_aton(cfg->my_ip_addr, &myAddr.sin_addr);

	temp = bind(media_bus.sock, (struct sockaddr *)&myAddr, sizeof(myAddr));
	if (temp < 0)
	{
		printf("could not bind or connect to socket, error = %d\n", temp);
		perror("");
		goto exit;
	}

	printf("listening on port %d, socket %u\n", cfg->my_listening_port, media_bus.sock);

exit:
	return 0;
}

int main()
{
	media_bus_init(&mbus_cfg);

	int err;
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;

	/* Open audio capture device */
    if ((err = snd_pcm_open (&capture_handle, "plughw:1,0", SND_PCM_STREAM_CAPTURE, 0)) < 0)
    {
        printf ("ERROR: cannot open audio capture device %s (%s)\n",
                 "plughw:1,0",
                 snd_strerror (err));
        exit (1);
    }

    printf("audio CAPTURE interface opened\n");

	/* Setting blocking mode - block until space is available in the buffer */
    if ((err = snd_pcm_nonblock (capture_handle, 0)) < 0) 
    {
        printf ("ERROR: cannot set block mod (%s)\n",
                snd_strerror (err));
        exit (1);
    }

    printf("set block mode\n");

	/* Allocate space for hardware parameters */
    if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) 
    {
        printf ("ERROR: cannot allocate hardware parameter structure (%s)\n",
                 snd_strerror (err));
        exit (1);
    }

    printf("hw_params allocated\n");

	/* Fill params with a full configuration space for a PCM */
    if ((err = snd_pcm_hw_params_any (capture_handle, hw_params)) < 0) 
    {
        printf ("ERROR: cannot initialize hardware parameter structure (%s)\n",
                 snd_strerror (err));
        exit (1);
    }

    printf("hw_params initialized\n");

	/* Config access mode for hardware parameters - r/w method */
    if ((err = snd_pcm_hw_params_set_access (capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) 
    {
        printf ("ERROR: cannot set access type (%s)\n",
                 snd_strerror (err));
        exit (1);
    }

    printf("hw_params access setted\n");

	/* Config PCM sample format */
    if ((err = snd_pcm_hw_params_set_format (capture_handle, hw_params, format)) < 0) 
    {
        printf ("ERROR: cannot set sample format (%s)\n",
                 snd_strerror (err));
        exit (1);
    }

    printf("hw_params format setted\n");

    /* Config sampling rate near to the target */
    if ((err = snd_pcm_hw_params_set_rate_near (capture_handle, hw_params, &rate, 0)) < 0) 
    {
        printf ("ERROR: cannot set sample rate (%s)\n",
                 snd_strerror (err));
        exit (1);
    }

    printf("hw_params rate setted\n");

    /* Config to use 2 channels - stereo */
    if ((err = snd_pcm_hw_params_set_channels (capture_handle, hw_params, 2)) < 0) 
    {
        printf ("ERROR: cannot set channel count (%s)\n",
                 snd_strerror (err));
        exit (1);
    }

    printf("hw_params channels setted\n");

    /* Set all above configs */
    if ((err = snd_pcm_hw_params (capture_handle, hw_params)) < 0) 
    {
        printf ("ERROR: cannot set parameters (%s)\n",
                 snd_strerror (err));
        exit (1);
    }

    printf("hw_params setted\n");

    /* Remove PCM hardware configuration and free associated resources/memory */
    snd_pcm_hw_params_free (hw_params);

    printf("hw_params freed\n");

    /* Prepare audio interface for use */
    if ((err = snd_pcm_prepare (capture_handle)) < 0) 
    {
        printf ("ERROR: cannot prepare audio interface for use (%s)\n",
                 snd_strerror (err));
        exit (1);
    }

    printf("audio interface prepared\n");

    return 0;
}