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
	.my_ip_addr = "192.168.100.103",     //static ip of RPi on my router
	.my_listening_port = 27772, 
	.sendto_ip_addr = "192.168.100.128", // dhcp ip of ESP32
	.sendto_port = 37773,
};

static char rcv_buff[UDP_PORTION_OF_SAMPLES_BYTES];
static char random_arr[UDP_PORTION_OF_SAMPLES_BYTES];

static int buffer_frames = CODEC_PORTION_OF_SAMPLES_CNT;
static unsigned int rate = CODEC_SAMPLING_RATE;
static snd_pcm_t *capture_handle;
static snd_pcm_t *playback_handle;
static char *buffer;

int main()
{
    return 0;
}