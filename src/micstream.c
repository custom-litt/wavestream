/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <soundio/soundio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/** the client socket file descriptor through which to stream values */
volatile int client_sockfd = -1;

/** the client address information (only one client at a time may be connected,
 * but this client may come and go) */
struct sockaddr_in client_addr;

/** socket file descriptor for the listening port */
volatile int server_sockfd = -1;

/** Forward declarations. */
static int start_server(
    char* hostname, int port_number, int listen_backlog,
    int* listening_sockfd_out);
static int try_send_chunk_to_client(char* chunk_buf, int length);
static int try_accept_client();

static void read_callback(
    struct SoundIoInStream* instream, int frame_count_min, int frame_count_max);
static void overflow_callback(struct SoundIoInStream* instream);

/**
 * @brief Shows proper usage of the program
 *
 * @param progname
 * @return int
 */
static int show_usage(char* progname) {
  int ret = 0;

  printf(
      "Usage: %s [options]\n"
      "Options:\n"
      "--hostname <hostname>: the hostname to use, defualts to \"localhost\"\n"
      "--port <port>: the port on which to expose the server. defaults to "
      "42311\n",
      progname);

out:
  return ret;
}

/** Returns the lesser of two ints. */
static int min_int(int a, int b) { return (a < b) ? a : b; }

struct RecordContext {
  struct SoundIoRingBuffer* ring_buffer;
};

static enum SoundIoFormat prioritized_formats[] = {
    SoundIoFormatS16NE,
    // SoundIoFormatS16FE,
    // SoundIoFormatU16NE,
    // SoundIoFormatU16FE,
    SoundIoFormatInvalid,
};

static int prioritized_sample_rates[] = {
    48000,
    // 44100,
    // 96000,
    // 24000,
    0,
};

int main(int argc, char** argv) {
  int err = 0;
  char* progname = argv[0];
  char* inputfilename = NULL;
  char* hostname = "localhost";
  int port_number = 42311;

  double latency = 0.0;

  // check the number of arguments
  if (argc < 1) {
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
      printf("ERROR: unknown argument \"%s\"\n", arg);
      exit(1);
    }
  }

  // start the server
  int server_sockfd;
  err = start_server(hostname, port_number, 5, &server_sockfd);
  if (0 != err) {
    fprintf(stderr, "ERROR: failed to start server\n");
    return 1;
  }

  // create a recording context
  struct RecordContext rc;
  struct SoundIo* soundio = soundio_create();
  if (!soundio) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  // connect to the specified backend, or the default
  enum SoundIoBackend backend = SoundIoBackendNone;
  char* device_id = NULL;
  err = (backend == SoundIoBackendNone)
                ? soundio_connect(soundio)
                : soundio_connect_backend(soundio, backend);
  if (err) {
    fprintf(stderr, "error connecting: %s", soundio_strerror(err));
    return 1;
  }

  // detect the connected backend and display to user
  soundio_flush_events(soundio);
  struct SoundIoDevice* selected_device = NULL;
  bool is_raw = false;
  if (device_id) {
    for (int i = 0; i < soundio_input_device_count(soundio); i += 1) {
      struct SoundIoDevice* device = soundio_get_input_device(soundio, i);
      if (device->is_raw == is_raw && strcmp(device->id, device_id) == 0) {
        selected_device = device;
        break;
      }
      soundio_device_unref(device);
    }
    if (!selected_device) {
      fprintf(stderr, "Invalid device id: %s\n", device_id);
      return 1;
    }
  } else {
    int device_index = soundio_default_input_device_index(soundio);
    selected_device = soundio_get_input_device(soundio, device_index);
    if (!selected_device) {
      fprintf(stderr, "No input devices available.\n");
      return 1;
    }
  }

  fprintf(stderr, "Device: %s\n", selected_device->name);

  if (selected_device->probe_error) {
    fprintf(
        stderr, "Unable to probe device: %s\n",
        soundio_strerror(selected_device->probe_error));
    return 1;
  }

  soundio_device_sort_channel_layouts(selected_device);

  // Check that the backend device supports the desired sample rate
  int sample_rate = 0;
  int* sample_rate_ptr;
  for (sample_rate_ptr = prioritized_sample_rates; *sample_rate_ptr;
       sample_rate_ptr += 1) {
    if (soundio_device_supports_sample_rate(
            selected_device, *sample_rate_ptr)) {
      sample_rate = *sample_rate_ptr;
      break;
    }
  }
  if (!sample_rate) {
    sample_rate = selected_device->sample_rates[0].max;
    fprintf(
        stderr,
        "ERROR: could not find a suitable sample rate from prioritized sample "
        "rates. To correct this error it is possible to use the default sample "
        "rate provided by the device, but this program is shutting down to "
        "avoid miscommunication over the socket (where sample rate is not "
        "conveyed).\n");
    exit(1);
  }

  // Check that the backend device supports the desired format
  enum SoundIoFormat fmt = SoundIoFormatInvalid;
  enum SoundIoFormat* fmt_ptr;
  for (fmt_ptr = prioritized_formats; *fmt_ptr != SoundIoFormatInvalid;
       fmt_ptr += 1) {
    if (soundio_device_supports_format(selected_device, *fmt_ptr)) {
      fmt = *fmt_ptr;
      break;
    }
  }
  if (fmt == SoundIoFormatInvalid) {
    fmt = selected_device->formats[0];
    fprintf(
        stderr,
        "ERROR: could not find a suitable format from prioritized formats. To "
        "correct this error it is possible to use the default format provided "
        "by the device, but this program is shutting down to avoid "
        "miscommunication over the socket (where format is not conveyed).\n");
    exit(1);
  }

  struct SoundIoInStream* instream = soundio_instream_create(selected_device);
  if (!instream) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  instream->format = fmt;
  instream->sample_rate = sample_rate;
  instream->read_callback = read_callback;
  instream->overflow_callback = overflow_callback;
  instream->userdata = &rc;

  if ((err = soundio_instream_open(instream))) {
    fprintf(stderr, "unable to open input stream: %s", soundio_strerror(err));
    return 1;
  }

  printf(
      "%s %dHz %s interleaved\n", instream->layout.name, sample_rate,
      soundio_format_string(fmt));

  const int ring_buffer_duration_seconds = 30;
  int capacity = ring_buffer_duration_seconds * instream->sample_rate *
                 instream->bytes_per_frame;
  rc.ring_buffer = soundio_ring_buffer_create(soundio, capacity);
  if (!rc.ring_buffer) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  if ((err = soundio_instream_start(instream))) {
    fprintf(stderr, "unable to start input device: %s", soundio_strerror(err));
    return 1;
  }

  // Note: in this example, if you send SIGINT (by pressing Ctrl+C for example)
  // you will lose up to 1 second of recorded audio data. In non-example code,
  // consider a better shutdown strategy.
  for (;;) {

    // check for client connections
    err = try_accept_client();
    if (0 != err) {
      fprintf(stderr, "ERROR trying to accept client\n");
      exit(1);
    }

    // update soundio event loop
    soundio_flush_events(soundio);


    // sleep(1);
    usleep(1000 * 100); // sleep for 100 msec (this determines the total number of samples collected)


    int fill_bytes = soundio_ring_buffer_fill_count(rc.ring_buffer);
    char* read_buf = soundio_ring_buffer_read_ptr(rc.ring_buffer);

    // send the chunk through the socket
    err = try_send_chunk_to_client(read_buf, fill_bytes);
    if (0 != err) {
      fprintf(stderr, "ERROR: failed to write chunk to client\n");
      break;
    }

    // advance the ring buffer by the number of bytes
    soundio_ring_buffer_advance_read_ptr(rc.ring_buffer, fill_bytes);
  }

  soundio_instream_destroy(instream);
  soundio_device_unref(selected_device);
  soundio_destroy(soundio);
  return 0;
}

/**
 * @brief This callback fills a user provided ringbuffer with the latest recording data
 * 
 * @param instream 
 * @param frame_count_min 
 * @param frame_count_max 
 */
static void read_callback(
    struct SoundIoInStream* instream, int frame_count_min,
    int frame_count_max) {
  struct RecordContext* rc = instream->userdata;
  struct SoundIoChannelArea* areas;
  int err;

  char* write_ptr = soundio_ring_buffer_write_ptr(rc->ring_buffer);
  int free_bytes = soundio_ring_buffer_free_count(rc->ring_buffer);
  int free_count = free_bytes / instream->bytes_per_frame;

  if (free_count < frame_count_min) {
    fprintf(stderr, "ring buffer overflow\n");
    exit(1);
  }

  int write_frames = min_int(free_count, frame_count_max);
  int frames_left = write_frames;

  for (;;) {
    int frame_count = frames_left;

    if ((err = soundio_instream_begin_read(instream, &areas, &frame_count))) {
      fprintf(stderr, "begin read error: %s", soundio_strerror(err));
      exit(1);
    }

    if (!frame_count) break;

    printf("bytes per sample: %d\n", instream->bytes_per_sample);

    if (!areas) {
      // Due to an overflow there is a hole. Fill the ring buffer with
      // silence for the size of the hole.
      memset(write_ptr, 0, frame_count * instream->bytes_per_frame);
    } else {
      for (int frame = 0; frame < frame_count; frame += 1) {
        for (int ch = 0; ch < instream->layout.channel_count; ch += 1) {
          memcpy(write_ptr, areas[ch].ptr, instream->bytes_per_sample);
          areas[ch].ptr += areas[ch].step;
          write_ptr += instream->bytes_per_sample;
        }
      }
    }

    if ((err = soundio_instream_end_read(instream))) {
      fprintf(stderr, "end read error: %s", soundio_strerror(err));
      exit(1);
    }

    frames_left -= frame_count;
    if (frames_left <= 0) break;
  }

  int advance_bytes = write_frames * instream->bytes_per_frame;
  soundio_ring_buffer_advance_write_ptr(rc->ring_buffer, advance_bytes);
}

static void overflow_callback(struct SoundIoInStream* instream) {
  static int count = 0;
  fprintf(stderr, "overflow %d\n", ++count);
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
  server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_sockfd < 0) {
    fprintf(stderr, "ERROR opening listening socket\n");
    ret = 1;
    goto out;
  }

  // make the server socket non-blocking
  // https://jameshfisher.com/2017/04/05/set_socket_nonblocking/
  int server_socketfd_flags = fcntl(server_sockfd, F_GETFL);
  if (server_socketfd_flags == -1) {
    fprintf(
        stderr, "ERROR getting listening socket flags (errno: %d)\n", errno);
  }
  ret = fcntl(server_sockfd, F_SETFL, server_socketfd_flags | O_NONBLOCK);
  if (ret == -1) {
    fprintf(
        stderr, "ERROR setting listening socket flags (errno: %d)\n", errno);
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
    fprintf(stderr, "ERROR on binding listening socket (errno: %d)\n", errno);
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

static int try_send_chunk_to_client(char* chunk_buf, int length) {
  if (NULL == chunk_buf) {
    return -ENOMEM;
  }

  // if there is no client to write to then just return okay
  if (client_sockfd == -1) {
    return 0;
  }

  // send the frames to the client connection
  int chars_to_send = length;
  int chars_sent = send(client_sockfd, (void*)chunk_buf, chars_to_send, 0);
  if (chars_sent == -1) {
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
      printf("Would have blocked, skipping this chunk\n");
      return 0;
    } else if (errno == EPIPE) {
      // TODO: figure out why this broken pipe condition stops the whole system
      // from continuing... expected behavior is that the server becomes
      // available again for another client to connect.
      int close_status = close(client_sockfd);
      if (0 != close_status) {
        fprintf(stderr, "SERIOUS ERROR: failed to close old file descriptor\n");
        exit(1);
      }

      errno = 0;
      printf("broken pipe, forgetting client\n");
      client_sockfd = -1;
      return 0;
    }

    client_sockfd = -1;
    fprintf(stderr, "Error sending data: %s\n", strerror(errno));
    return 1;
  }
  if (chars_sent != chars_to_send) {
    fprintf(
        stderr, "ERROR: expected to send %d chars but actually sent %d.\n",
        chars_to_send, chars_sent);
    return 1;
  }

  return 0;
}

/**
 * @brief Tries to accept a client, if available.
 *
 */
static int try_accept_client() {
  // check for already connected client
  if (client_sockfd != -1) {
    return 0;
  }

  // if no client connected then try to accept another
  int client_addr_len = sizeof(client_addr);
  bzero((char*)&client_addr, sizeof(client_addr));
  int ret =
      accept(server_sockfd, (struct sockaddr*)&client_addr, &client_addr_len);
  if (ret == -1) {
    client_sockfd = ret;
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
      // there was no client available to accept, this is allowed
      errno = 0;
      return 0;
    } else {
      fprintf(stderr, "ERROR: failed to accept the client\n");
      return ret;
    }
  } else {
    client_sockfd = ret;
    ret = 0;
    printf(
        "connected to client: %d (%d)\n", client_sockfd, client_addr.sin_port);
    return 0;
  }
}
