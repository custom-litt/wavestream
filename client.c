/**
 * @file client.c
 * @author oclyke
 * @brief sucks in data from a server.
 */

#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

static int show_usage(char* progname);

int main(int argc, char* argv[]) {
  // set some initial values
  int ret = 0;
  char* hostname = "localhost";
  int port_number = 42310;

  // parse arguments
  // - the supplied arguments always begins with the name of the program
  // argv[0] = program name
  // argv[1 ... n] = a string for each space-separated argument on the command
  // line
  char* progname = argv[0];

  // there are no required arguments
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
    }
  }

  // construct a socket to be used in connection mode
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    fprintf(stderr, "ERROR creating socket\n");
    return 1;
  }

  // get server information
  struct hostent* server = gethostbyname(hostname);
  if (server == NULL) {
    fprintf(stderr, "ERROR, no such host\n");
    return 1;
  }

  // get server address information
  struct sockaddr_in serv_addr;
  bzero((char*)&serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  bcopy(
      (char*)server->h_addr, (char*)&serv_addr.sin_addr.s_addr,
      server->h_length);
  serv_addr.sin_port = htons(port_number);

  // connect the socket to the server
  printf("connecting to server at %s:%d\n", hostname, port_number);
  ret = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
  if (ret < 0) {
    fprintf(stderr, "ERROR connecting to server\n");
    return 1;
  }
  printf("connected!\n");

  // suck in data from the server
  const size_t rx_buffer_len = 4096;
  char rx_buffer[rx_buffer_len];
  int chars_received = -1;
  do {
    // thanks ChatGPT for spotting my variable-shadowing issue
    chars_received = recv(sockfd, &rx_buffer, rx_buffer_len, 0);

    if (chars_received == -1) {
        // an error has occurred
        fprintf(stderr, "Error receiving data: %s\n", strerror(errno));
        continue;
    } else if (chars_received == 0) {
        // remote peer has closed the connection
        printf("Remote peer has closed the connection.\n");
        continue;
    } else {
        // data received successfully
        printf("Received %d bytes of data.\n", chars_received);
    }

  } while (chars_received > 0);

  if (chars_received < 0) {
    fprintf(stderr, "ERROR receiving message (return code: %d, errno: %d)\n", chars_received, errno);
    return 1;
  }
  if (chars_received == 0) {
    printf("stream ended");
  }

  return 0;
}

static int show_usage(char* progname) {
  int ret = 0;

  printf(
      "Usage: %s [options] <listening port number>\n"
      "Options:\n"
      "--hostname <hostname>: the hostname to use, defaults to \"localhost\"\n"
      "--port <port number>: the port to connect to, defaults to 42310\n",
      progname);

out:
  return ret;
}
