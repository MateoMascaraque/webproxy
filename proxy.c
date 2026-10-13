#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netdb.h>
#include <netinet/in.h>
#include <errno.h>
#include <netinet/tcp.h>

#define QUEUE_LENGTH 10
#define BUFFER_SIZE 4096
#define MAX_REQUEST_LEN 65535 // same max length that ParsedRequest_parse accepts
#define HOST_HEADER_LEN 512   // room for "host:port"

// ERROR RESPONSES //
#define RESPONSE_400 "HTTP/1.0 400 Bad Request\r\n\r\n"
#define RESPONSE_501 "HTTP/1.0 501 Not Implemented\r\n\r\n"
#define RESPONSE_502 "HTTP/1.0 502 Bad Gateway\r\n\r\n"

// real HTTP methods that are not GET -> 501
static const char *other_methods[] = {
  "HEAD", "POST", "PUT", "DELETE", "LINK", "UNLINK",
  "OPTIONS", "TRACE", "CONNECT", "PATCH", NULL // NULL marks the end of the list
};


// send_all()
// same as first assignemtn
static int send_all(int socket_descriptor, const char *buff, size_t len) {
  size_t total = 0;

  while (total < len) {
    // MSG_NOSIGNAL: if the other side already closed, send() returns -1 (to not crash)
    ssize_t bytes_sent = send(socket_descriptor, buff + total, len - total, MSG_NOSIGNAL);
    if (bytes_sent == -1) {
      if (errno == EINTR) {
        continue; // interrupted, retry
      }
      return -1;
    }

    total += bytes_sent;
  }

  return 0;
}


// find_header_end()
// look for "\r\n\r\n", cant look as a string [why?]
static size_t find_header_end(const char *buff, size_t len) {
  for (size_t i = 0; i + 3 < len; i++) {
    if (buff[i] == '\r' && buff[i + 1] == '\n' &&
        buff[i + 2] == '\r' && buff[i + 3] == '\n') {
      return i + 4;
    }
  }
  return 0;
}


// read_request()
static int read_request(int client_descriptor, char *buff, size_t buff_size) {
  size_t used = 0; // len of buffer content

  while (1) {
    // buffer full and still no end of headers
    if (used == buff_size) {
      return -1;
    }

    ssize_t len = recv(client_descriptor, buff + used, buff_size - used, 0);
    if (len == -1 && errno == EINTR) {
      continue;
    }
    if (len <= 0) {
      return 0; // client closed or error
    }

    // "\r\n\r\n" CAN be separated in two recv()
    // I have to check the previous 3 (if it were 4 the whole end would be there)
    size_t start = 0;
    if (used >= 3) {
      start = used - 3;
    }
    used += len;

    size_t end = find_header_end(buff + start, used - start);
    if (end > 0) {
      return start + end;
    }
  }
}


// is_other_method() than GET (1 is not GET, 0 is get)
static int is_other_method(const char *method) {
  for (int i = 0; other_methods[i] != NULL; i++) {
    if (strcmp(method, other_methods[i]) == 0) {
      return 1;
    }
  }
  return 0;
}


// remove_header()
// made case insensnitive (strcasecmp)

static void remove_header(struct ParsedRequest *req, const char *name) {
  for (size_t i = 0; i < req->headersused; i++) {
    struct ParsedHeader *header = req->headers + i;
    // skip removed headers
    if (header->key != NULL && strcasecmp(header->key, name) == 0) {
      ParsedHeader_remove(req, header->key);
    }
  }
}


// build_server_request()
/* Request
GET <path> HTTP/1.0\r\n
<client headers>\r\n
Host: <host>[:<port>]\r\n
Connection: close\r\n
\r\n
*/
static char *build_server_request(struct ParsedRequest *req, size_t *out_len) {
  // HEADERS //
  // replace the client's Host and Connection with ours (any upper/lower case)
  remove_header(req, "Host");
  remove_header(req, "Connection");

  // Host comes from the URL, with the port only if the URL had one
  char host_value[HOST_HEADER_LEN];
  if (req->port != NULL) {
    snprintf(host_value, sizeof host_value, "%s:%s", req->host, req->port);
  } else {
    snprintf(host_value, sizeof host_value, "%s", req->host);
  }

  if (ParsedHeader_set(req, "Host", host_value) < 0 ||
      ParsedHeader_set(req, "Connection", "close") < 0) {
    return NULL;
  }

  // REQUEST LINE //
  // ParsedRequest_unparse() writes full URL (http://host/path), but server requieres only the path
  char request_line[MAX_REQUEST_LEN];
  int line_len = snprintf(request_line, sizeof request_line, "%s %s HTTP/1.0\r\n",
                          req->method, req->path);
  if (line_len < 0 || (size_t)line_len >= sizeof request_line) {
    return NULL;
  }

  // PUT IT TOGETHER //
  // headersLen counts all headers plus the final "\r\n"
  size_t headers_len = ParsedHeader_headersLen(req);
  char *out = malloc(line_len + headers_len);
  if (out == NULL) {
    return NULL;
  }

  memcpy(out, request_line, line_len);
  if (ParsedRequest_unparse_headers(req, out + line_len, headers_len) < 0) {
    free(out);
    return NULL;
  }

  *out_len = line_len + headers_len;
  return out;
}


// connect_to_server()
// Look up the host's IPv4 address, then open a TCP connection to it (takes names or ips)
static int connect_to_server(const char *host, const char *port) {
  // LOOK UP THE HOST //
  struct hostent *server = gethostbyname(host);
  if (server == NULL) {
    return -1; // unknown host
  }

  // FILL IN THE SERVER ADDRESS //
  struct sockaddr_in server_address;
  memset(&server_address, 0, sizeof server_address);
  server_address.sin_family = AF_INET; // IPv4
  server_address.sin_port = htons(atoi(port)); // port in network byte order
  memcpy(&server_address.sin_addr, server->h_addr_list[0], server->h_length); // first IP of the host

  // SOCKET + CONNECT //
  int server_descriptor = socket(AF_INET, SOCK_STREAM, 0); // TCP
  if (server_descriptor < 0) {
    return -1;
  }

  if (connect(server_descriptor, (struct sockaddr *)&server_address, sizeof server_address) < 0) {
    close(server_descriptor);
    return -1;
  }

  return server_descriptor;
}


// relayresponse()
// forward from server to the client
static void relay_response(int server_descriptor, int client_descriptor) {
  char buff[BUFFER_SIZE];

  while (1) {
    ssize_t len = recv(server_descriptor, buff, sizeof buff, 0);
    if (len == -1 && errno == EINTR) {
      continue;
    }
    if (len <= 0) {
      return; // server finished (0) or error (<0)
    }

    if (send_all(client_descriptor, buff, len) == -1) {
      return; // no cliend
    }
  }
}


// handle_client()
// steps carried do 
static void handle_client(int client_descriptor) {
  // READ //
  char request[MAX_REQUEST_LEN];
  int request_len = read_request(client_descriptor, request, sizeof request);
  
  if (request_len == 0) {
    return; // client left before finishing the request
  }
  if (request_len < 0) {
    send_all(client_descriptor, RESPONSE_400, strlen(RESPONSE_400));
    return;
  }

  // PARSE //
  // the library checks the request line and that headers are "Name: value"
  struct ParsedRequest *req = ParsedRequest_create();
  
  if (req == NULL) {
    return;
  }

  if (ParsedRequest_parse(req, request, request_len) < 0) {
    send_all(client_descriptor, RESPONSE_400, strlen(RESPONSE_400));
    ParsedRequest_destroy(req);
    return;
  }

  // CHECK METHOD //
  // only GET
  if (strcmp(req->method, "GET") != 0) {
    
    if (is_other_method(req->method)) {
      send_all(client_descriptor, RESPONSE_501, strlen(RESPONSE_501));
    } else {
      send_all(client_descriptor, RESPONSE_400, strlen(RESPONSE_400));
    }
    
    ParsedRequest_destroy(req);
    return;
  }

  // BUILD REQUEST FOR SERVER //
  size_t out_len;
  char *out = build_server_request(req, &out_len);
  
  if (out == NULL) {
    send_all(client_descriptor, RESPONSE_400, strlen(RESPONSE_400));
    ParsedRequest_destroy(req);
    return;
  }

  // CONNECT TO SERVER //
  // if none given, default HTTP port 80
  char *port = "80";
  
  if (req->port != NULL) {
    port = req->port;
  }
  
  int server_descriptor = connect_to_server(req->host, port);
  ParsedRequest_destroy(req); // don't need host/port anymore

  if (server_descriptor == -1) {
    // not essential, but let know when closing
    send_all(client_descriptor, RESPONSE_502, strlen(RESPONSE_502));
    free(out);
    return;
  }

  // SEND AND RELAY //
  if (send_all(server_descriptor, out, out_len) == 0) {
    relay_response(server_descriptor, client_descriptor);
  }

  free(out);
  close(server_descriptor);
}


// reap_children()
// ends subprocesses
static void reap_children(int sig) {
  (void)sig; // not used, avoids the unused parameter warning
  int saved_errno = errno; // waitpid can change errno under the code we interrupted

  while (waitpid(-1, NULL, WNOHANG) > 0) {
    // the reapingg happens in the condition, repear until no children
  }

  errno = saved_errno;
}


//proxy()
// server-like socket, accept requests get data and forward
int proxy(char *proxy_port) {
  // CHILDREN CLEANUP //
  // call reap_children every time a child ends
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = reap_children;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART; // so accept() keeps going after the handler

  if (sigaction(SIGCHLD, &sa, NULL) == -1) {
    perror("sigaction");
    return 1;
  }

  // GET ADDRESS INFO //
  // first assignment copy-paste
  struct addrinfo hints;
  struct addrinfo *servinfo;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  int status = getaddrinfo(NULL, proxy_port, &hints, &servinfo);
  if (status != 0) {
    fprintf(stderr, "ERROR getaddrinfo - %s\n", gai_strerror(status));
    return 1;
  }

  // SOCKET + SETSOCKOPT + BIND //
  int socket_descriptor = -1;
  for (struct addrinfo *p = servinfo; p != NULL; p = p->ai_next) {
    socket_descriptor = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (socket_descriptor < 0) {
      perror("socket");
      continue;
    }

    // So that I dont get the already in use error
    int optval = 1;
    if (setsockopt(socket_descriptor, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval) < 0) {
      perror("setsockopt");
      close(socket_descriptor);
      socket_descriptor = -1;
      continue;
    }

    if (bind(socket_descriptor, p->ai_addr, p->ai_addrlen) < 0) {
      perror("bind");
      close(socket_descriptor);
      socket_descriptor = -1;
      continue;
    }

    // if we got here it means we made a bind
    break;
  }

  freeaddrinfo(servinfo); // re-give

  if (socket_descriptor == -1) {
    fprintf(stderr, "ERROR: could not bind to port %s\n", proxy_port);
    return 1;
  }

  // LISTEN //
  if (listen(socket_descriptor, QUEUE_LENGTH) < 0) {
    perror("listen");
    close(socket_descriptor);
    return 1;
  }

  // ACCEPT + FORK //
  // one child process per client, so clients don't wait for each other
  while (1) {
    int client_descriptor = accept(socket_descriptor, NULL, NULL);
    if (client_descriptor == -1) {
      continue; // one bad client shouldn't stop the proxy
    }

    pid_t pid = fork();

    if (pid == 0) {
      // CHILD //
      // it only talks to this client, so it doesn't need the listening socket
      close(socket_descriptor);
      handle_client(client_descriptor);
      close(client_descriptor);
      exit(0);
    }

    // PARENT //
    // close childs client descriptor
    close(client_descriptor);
  }

  close(socket_descriptor);
  return 0;
}


int main(int argc, char * argv[]) {
  char *proxy_port;

  if (argc != 2) {
    fprintf(stderr, "Usage: ./proxy <port>\n");
    exit(EXIT_FAILURE);
  }

  proxy_port = argv[1];
  return proxy(proxy_port);
}
