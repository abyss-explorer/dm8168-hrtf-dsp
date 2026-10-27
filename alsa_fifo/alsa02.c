/*

This example reads from the default PCM device
and writes to standard output for 5 seconds of data.

*/

/* Use the newer ALSA API */
#define ALSA_PCM_NEW_HW_PARAMS_API

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
static int keep_running = 1;
void sig_handler(int sig) { keep_running = 0; }

int main() {
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	long loops;
	int rc;
	int size;
	snd_pcm_t *handle;
	snd_pcm_hw_params_t *params;
	unsigned int val;
	int dir;
	snd_pcm_uframes_t frames;
	char *buffer;
	
	/* Open PCM device for recording (capture). */
	rc = snd_pcm_open(&handle, "default",
		            SND_PCM_STREAM_CAPTURE, 0);
	if (rc < 0) {
	    fprintf(stderr,
		    "unable to open pcm device: %s\n",
		    snd_strerror(rc));
	    exit(1);
	}

	/* Allocate a hardware parameters object. */
	snd_pcm_hw_params_malloc(&params);

	/* Fill it in with default values. */
	snd_pcm_hw_params_any(handle, params);

	/* Set the desired hardware parameters. */

	/* Interleaved mode */
	snd_pcm_hw_params_set_access(handle, params,
		              SND_PCM_ACCESS_RW_INTERLEAVED);

	/* Signed 16-bit little-endian format */
	snd_pcm_hw_params_set_format(handle, params,
		                      SND_PCM_FORMAT_S16_LE);

	/* Two channels (stereo) */
	snd_pcm_hw_params_set_channels(handle, params, 2);

	/* 44100 bits/second sampling rate (CD quality) */
	val = 48000;
	snd_pcm_hw_params_set_rate_near(handle, params,
		                          &val, &dir);

	/* Set period size to 32 frames. */
	frames = 32;
	snd_pcm_hw_params_set_period_size_near(handle,
		                      params, &frames, &dir);

	/* Write the parameters to the driver */
	rc = snd_pcm_hw_params(handle, params);
	if (rc < 0) {
	    fprintf(stderr,
		    "unable to set hw parameters: %s\n",
		    snd_strerror(rc));
	    exit(1);
	}

	/* Use a buffer large enough to hold one period */
	snd_pcm_hw_params_get_period_size(params,
		                              &frames, &dir);
	size = frames * 4; /* 2 bytes/sample, 2 channels */
	buffer = (char *) malloc(size);

	/* We want to loop for 5 seconds */
	snd_pcm_hw_params_get_period_time(params,
		                                 &val, &dir);
	loops = 4000000 / val;  //8k_sameple/s, 32 samples, --> 4ms, so 1000 loops =  4 seconds recording....
	while (keep_running)
	{
	    loops--;
	    rc = snd_pcm_readi(handle, buffer, frames);
	    if (rc == -EPIPE) {
	      /* EPIPE means overrun */
	      fprintf(stderr, "overrun occurred\n");
	      snd_pcm_prepare(handle);
	    } else if (rc < 0) {
	      fprintf(stderr,
		      "error from read: %s\n",
		      snd_strerror(rc));
	    } else if (rc != (int)frames) {
	      fprintf(stderr, "short read, read %d frames\n", rc);
	    }
	    ssize_t bytes_written = write(1, buffer, size);
		if (bytes_written == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue; 
			} else {
				perror("write to fifo failed");
				break;
			}
		} else if (bytes_written != size) {
			fprintf(stderr, "short write: wrote %zd bytes (expected %d)\n", bytes_written, size);
		}
	}

	snd_pcm_drain(handle);
	snd_pcm_close(handle);
	free(buffer);
	return 0;
}
