/**
 * @file wavestream.c
 * @author oclyke
 * @brief play a WAV file while also serving to clients.
 * 
 */

#include <soundio/soundio.h>
#include <sndfile.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/** the block size that determines the maximum atomic reads from the file */
#define	BLOCK_SIZE (4096)

/**
 * global variables
 */

/** info about the soundfile */
SF_INFO sfinfo;

/** the sound file pointer */
SNDFILE *infile = NULL;

/** buffer where chunks of sound file will be converted to floats to be sent through libsoundio output*/
double* snd_buf = NULL;

/** function pointer used to select an appropriate write method for the selected backend */
static void (*write_sample)(char *ptr, double sample);

/** not sure how this is used at the moment */
static volatile bool want_pause = false;

/** the client socket file descriptor through which to stream values */
int client_sockfd = -1;

/**
 * forward declarations
 */
static int show_usage(char* progname);
static int start_server(
    char* hostname, int port_number, int listen_backlog,
    int* listening_sockfd_out);

static void write_sample_s16ne(char *ptr, double sample);
static void write_sample_s32ne(char *ptr, double sample);
static void write_sample_float32ne(char *ptr, double sample);
static void write_sample_float64ne(char *ptr, double sample);
static void write_callback(struct SoundIoOutStream *outstream, int frame_count_min, int frame_count_max);
static void underflow_callback(struct SoundIoOutStream *outstream);
static int send_chunk_to_client(int sockfd, double* chunk_buf, int chunk_len);

static int show_usage(char* progname) {
  int ret = 0;

  printf(
      "Usage: %s [options] <input file>\n"
      "Options:\n"
      "--hostname <hostname>: the hostname to use, defualts to \"localhost\"\n"
      "--port <port>: the port on which to expose the server. defaults to 42310\n",
      progname);

out:
  return ret;
}

int main(int argc, char **argv) {
    int ret = 0;
    char *progname = argv[0];
    char* inputfilename = NULL;
    char* hostname = "localhost";
    int port_number = 42310;
    double latency = 0.0;
    int sample_rate = 0;

    // check the number of arguments
    if (argc < 2) {
        fprintf(stderr, "ERROR: not enough arguments supplied\n");
        show_usage(progname);
        return 1;
	}

    // parse all arguments after the program name
    for (int idx = 1; idx < argc; idx++) {
        char* arg = argv[idx];
        if (strcmp(arg, "--hostname") == 0) {
            idx++;
            hostname = argv[idx];
        } else if (strcmp(arg, "--port") == 0) {
            idx++;
            port_number = atoi(argv[idx]);
        } else {
            inputfilename = arg;
        }
    }

    // show user the arguments
    printf("input file: %s\n", inputfilename);

    // start the server
    // stop_server should be called upon exit after start_server was successfully
    printf("Starting server at %s:%d\n", hostname, port_number);
    int server_sockfd;
    ret = start_server(hostname, port_number, 5, &server_sockfd);
    if (0 != ret) {
        fprintf(stderr, "ERROR: failed to start server\n");
        return 1;
    }

    // accept a connection
    // (but for now only one connection)
    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);
    client_sockfd =
        accept(server_sockfd, (struct sockaddr*)&client_addr, &client_addr_len);
    if (client_sockfd < 0) {
      fprintf(stderr, "ERROR: failed to accept the client\n");
      return 1;
    }
    printf(
        "connected to client: %d (%d)\n", client_sockfd, client_addr.sin_port);

    // get wav file to play
	char* infilename = argv[1];
	if (infilename [0] == '-') {
        printf ("Error : Input filename (%s) looks like an option.\n\n", infilename) ;
		return show_usage (progname);
	};

    // open the wav file
    memset (&sfinfo, 0, sizeof (sfinfo)) ;
    infile = sf_open(infilename, SFM_READ, &sfinfo);
	if (NULL == infile) {
        printf ("Not able to open input file %s.\n", infilename);
		puts (sf_strerror (NULL));
		goto cleanup_sf;
	};

    struct SoundIo *soundio = soundio_create();
    if (!soundio) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    // allocate the buffer for samples from the file
	snd_buf = malloc (BLOCK_SIZE * sizeof (double)) ;
	if (snd_buf == NULL) {
        fprintf (stderr, "Error : Out of memory.\n\n");
		return 1;
	};

    // get the soundio backend to use
    // by default this will try backends in order and select the first that is successful
    enum SoundIoBackend backend = SoundIoBackendNone;
    ret = (backend == SoundIoBackendNone) ? soundio_connect(soundio) : soundio_connect_backend(soundio, backend);
    if (0 != ret) {
        fprintf(stderr, "Unable to connect to backend: %s\n", soundio_strerror(ret));
        return 1;
    }
    printf("Backend: %s\n", soundio_backend_name(soundio->current_backend));

    // with an up-to-date list of output devices select the first that works with the backend
    soundio_flush_events(soundio); // not sure why this is necessary - I think it is meant to drive actions such as updating lists of connected devices etc...
    int selected_device_index = -1;
    selected_device_index = soundio_default_output_device_index(soundio);
    if (selected_device_index < 0) {
        fprintf(stderr, "Output device not found\n");
        return 1;
    }

    // expose the output device that will be used for writing
    struct SoundIoDevice *device = soundio_get_output_device(soundio, selected_device_index);
    if (!device) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    printf("Output device: %s\n", device->name);

    // check that the device can be probed
    if (device->probe_error) {
        fprintf(stderr, "Cannot probe device: %s\n", soundio_strerror(device->probe_error));
        return 1;
    }

    // create an output stream
    struct SoundIoOutStream *outstream = soundio_outstream_create(device);
    if (!outstream) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    outstream->write_callback = write_callback;
    outstream->underflow_callback = underflow_callback;
    outstream->name = NULL;
    outstream->software_latency = latency;
    outstream->sample_rate = sample_rate;

    // set the outstream sample rate from the data read by libsndfile
    outstream->sample_rate = sfinfo.samplerate;

    // select the appropriate format for writing samples
    if (soundio_device_supports_format(device, SoundIoFormatFloat32NE)) {
        outstream->format = SoundIoFormatFloat32NE;
        write_sample = write_sample_float32ne;
    } else if (soundio_device_supports_format(device, SoundIoFormatFloat64NE)) {
        outstream->format = SoundIoFormatFloat64NE;
        write_sample = write_sample_float64ne;
    } else if (soundio_device_supports_format(device, SoundIoFormatS32NE)) {
        outstream->format = SoundIoFormatS32NE;
        write_sample = write_sample_s32ne;
    } else if (soundio_device_supports_format(device, SoundIoFormatS16NE)) {
        outstream->format = SoundIoFormatS16NE;
        write_sample = write_sample_s16ne;
    } else {
        fprintf(stderr, "No suitable device format available.\n");
        return 1;
    }

    // open the output stream
    if ((ret = soundio_outstream_open(outstream))) {
        fprintf(stderr, "unable to open device: %s", soundio_strerror(ret));
        return 1;
    }
    fprintf(stderr, "Software latency: %f\n", outstream->software_latency);
    fprintf(stderr,
            "'p\\n' - pause\n"
            "'u\\n' - unpause\n"
            "'P\\n' - pause from within callback\n"
            "'c\\n' - clear buffer\n"
            "'q\\n' - quit\n");

    if (outstream->layout_error) {
        fprintf(stderr, "unable to set channel layout: %s\n", soundio_strerror(outstream->layout_error));
    }
    if ((ret = soundio_outstream_start(outstream))) {
        fprintf(stderr, "unable to start device: %s\n", soundio_strerror(ret));
        return 1;
    }

    for (;;) {
        soundio_flush_events(soundio);
        int c = getc(stdin);
        if (c == 'p') {
            // pause
            int result = soundio_outstream_pause(outstream, true);
            fprintf(stderr, "pausing result: %s\n", soundio_strerror(result));
        } else if (c == 'P') {
            // want pause??
            want_pause = true;
        } else if (c == 'u') {
            want_pause = false;
            int result = soundio_outstream_pause(outstream, false);
            fprintf(stderr, "unpausing result: %s\n", soundio_strerror(result));
        } else if (c == 'c') {
            int result = soundio_outstream_clear_buffer(outstream);
            fprintf(stderr, "clear buffer result: %s\n", soundio_strerror(result));
        } else if (c == 'q') {
            // quite
            break;
        } else if (c == '\r' || c == '\n') {
            // ignore
        } else {
            fprintf(stderr, "Unrecognized command: %c\n", c);
        }
    }

    soundio_outstream_destroy(outstream);
    soundio_device_unref(device);
    soundio_destroy(soundio);
cleanup_sf:
	sf_close (infile) ;

    return ret;
}

/**
 * @brief starts a server
 *
 * @param hostname a string used to determine the host. should probably be one
 * of "localhost" or "0.0.0.0" or "127.0.0.1" to start a server on the device
 * @param port_number the port at which the listening socket will be opened.
 * this is the port number that clients will specify to establish a connection
 * @param listen_backlog the back
 * @param listening_sockfd_out this is an output that gives access to the file
 * descriptor of the opened socket.
 * @return int
 */
static int start_server(
    char* hostname, int port_number, int listen_backlog,
    int* listening_sockfd_out) {
  // https://blog.stephencleary.com/2009/05/using-socket-as-server-listening-socket.html
  int ret = 0;

  // construct the listening socket
  // the server will establish a *listening* socket - this socket is only used
  // to listen for incoming connections
  int server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_sockfd < 0) {
    fprintf(stderr, "ERROR opening listening socket\n");
    ret = 1;
    goto out;
  }

  // bind the listening socket
  // binding on a listening socket is usually only done on the port with
  // the IP address set to "any" (??? is this to allow any IP address to
  // connect? does this mean you could whitelist an IP by setting it here?)
  struct sockaddr_in serv_addr;
  bzero((char*)&serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port_number);
  ret = bind(server_sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
  if (ret < 0) {
    fprintf(stderr, "ERROR on binding listening socket\n");
    goto out;
  }

  // start listening on the socket
  // this makes the port available for clients to try to establish a connection
  // "the listening socket actually begins listening at this point. it is not
  // yet accepting connections but the OS may accept connections on its behalf."
  ret = listen(server_sockfd, listen_backlog);
  if (0 != ret) {
    fprintf(stderr, "ERROR listening on the socket\n");
    goto out;
  }

  if (NULL != listening_sockfd_out) {
    *listening_sockfd_out = server_sockfd;
  }

out:
  return ret;
}

// writing functions
static void write_sample_s16ne(char *ptr, double sample) {
    int16_t *buf = (int16_t *)ptr;
    double range = (double)INT16_MAX - (double)INT16_MIN;
    double val = sample * range / 2.0;
    *buf = val;
}

static void write_sample_s32ne(char *ptr, double sample) {
    int32_t *buf = (int32_t *)ptr;
    double range = (double)INT32_MAX - (double)INT32_MIN;
    double val = sample * range / 2.0;
    *buf = val;
}

static void write_sample_float32ne(char *ptr, double sample) {
    float *buf = (float *)ptr;
    *buf = sample;
}

static void write_sample_float64ne(char *ptr, double sample) {
    double *buf = (double *)ptr;
    *buf = sample;
}

/**
 * @brief Called when the outputstream wants to request additional samples
 * 
 * @param outstream 
 * @param frame_count_min 
 * @param frame_count_max 
 */
static void write_callback(struct SoundIoOutStream *outstream, int frame_count_min, int frame_count_max) {
    struct SoundIoChannelArea *areas;
    int ret = 0;

    // attempt to feed the maximum number of frames
    // (one frame includes a sample for every channel)
    // output: single channel
    // input (file): channel count from sfinfo struct
    int frames_left = frame_count_max;
    int frame_count;

    for (;;) {
        frame_count = frames_left;

        // restrict the number of frames to handle in this chunk to the size
        // of the buffer used to read from the file
        int allowed_frames = BLOCK_SIZE / sfinfo.channels;
        if (frame_count > allowed_frames) {
            frame_count = allowed_frames;
        }
        // check for 0 frame count
        if (!frame_count) {
            break;
        }

        // read new data into the buffer
        int file_readcount = (int) sf_readf_double (infile, snd_buf, frame_count);
        if (file_readcount <= 0) {
            break;
        }
        if (file_readcount != frame_count) {
            fprintf(stderr, "ERROR: mismatch between read frames and requested frames\n");
            break;
        }

        // send the chunk through the socket
        ret = send_chunk_to_client(client_sockfd, snd_buf, frame_count);
        if (0 != ret) {
            fprintf(stderr, "ERROR: failed to write chunk to client\n");
            break;
        }

        // start writing to the output stream
        ret = soundio_outstream_begin_write(outstream, &areas, &frame_count);
        if (0 != ret) {
            fprintf(stderr, "unrecoverable stream error: %s\n", soundio_strerror(ret));
            exit(1);
        }

        // expose info about the output
        const struct SoundIoChannelLayout *layout = &outstream->layout;

        // actually set channel data using the proper write_samples function
        for (int frame = 0; frame < frame_count; frame += 1) {
            double sample = snd_buf[frame * sfinfo.channels + 0]; // get the sample from just one channel of the input audio... this could later be improved

            for (int channel = 0; channel < layout->channel_count; channel += 1) {
                write_sample(areas[channel].ptr, sample);
                areas[channel].ptr += areas[channel].step;
            }
        }

        // stop writing to the output stream
        ret = soundio_outstream_end_write(outstream);
        if (0 != ret) {
            if (ret == SoundIoErrorUnderflow) {
                return;
            }
            fprintf(stderr, "unrecoverable stream error: %s\n", soundio_strerror(ret));
            exit(1);
        }

        // decrement the number of frames that need to be pushed into the output
        frames_left -= frame_count;
        if (frames_left <= 0) {
            break;
        }
    }

    soundio_outstream_pause(outstream, want_pause);
}

/**
 * @brief Used when the outstream is not supplied with enough samples.
 * 
 * @param outstream 
 */
static void underflow_callback(struct SoundIoOutStream *outstream) {
    static int count = 0;
    fprintf(stderr, "underflow %d\n", count++);
}

static int send_chunk_to_client(int sockfd, double* chunk_buf, int chunk_len) {
    if (NULL == chunk_buf) {
        return -ENOMEM;
    }

    // send the frames to the client connection
    int chars_to_send = chunk_len * sizeof(double)/sizeof(char);
    int chars_sent = send(sockfd, (void*)chunk_buf, chars_to_send, 0);
    if (chars_sent == -1) {
        fprintf(stderr, "Error sending data: %s\n", strerror(errno));
        return 1;
    }
    if (chars_sent != chars_to_send) {
        fprintf(stderr, "ERROR: expected to send %d chars but actually sent %d.\n", chars_to_send, chars_sent);
        return 1;
    }

    return 0;
}
