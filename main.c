#define NET_IMPLEMENTATION
#include "lib/mininet.h"
#include "lib/isr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #define ISR_STAT _stat64
  #define ISR_STRUCT_STAT struct __stat64
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #define ISR_STAT stat
  #define ISR_STRUCT_STAT struct stat
#endif

#define DEFAULT_PORT "42069"

static void usage(void) {
  fprintf(stderr,
    "Usage:\n"
    "  isr serve <directory> [-p port]\n"
    "  isr send <host:port> <local_file> <remote_path> [-c blocksize] [-o]\n"
    "  isr recv <host:port> <remote_path> [local_path] [-c blocksize]\n"
    "\n"
    "Options:\n"
    "  -p port       Port to listen on (default: " DEFAULT_PORT ")\n"
    "  -c blocksize  Enable LZ4 compression with given block size in KB\n"
    "  -o            Overwrite existing files\n"
  );
}

// Parse "host:port" into separate host and port strings.
// Returns 0 on success, -1 on failure.
static int parse_host_port(const char* input, char* host, size_t host_len,
                            char* port, size_t port_len) {
  const char* colon = strrchr(input, ':');
  if (!colon || colon == input) return -1;

  size_t hlen = (size_t)(colon - input);
  if (hlen >= host_len) return -1;
  memcpy(host, input, hlen);
  host[hlen] = '\0';

  const char* p = colon + 1;
  if (strlen(p) == 0 || strlen(p) >= port_len) return -1;
  strcpy(port, p);
  return 0;
}

// Helper to recv exactly n bytes (for client-side use in main.c)
static int recv_exact(net_sock_t sock, void* buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    int r = net_recv(sock, (char*)buf + total, len - total);
    if (r <= 0) return -1;
    total += (size_t)r;
  }
  return 0;
}

static int cmd_serve(int argc, char* argv[]) {
  const char* directory = NULL;
  const char* port = DEFAULT_PORT;

  // Parse args
  if (argc < 1) { usage(); return 1; }
  directory = argv[0];
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      port = argv[++i];
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      usage();
      return 1;
    }
  }

  // Validate directory
  ISR_STRUCT_STAT st;
  if (ISR_STAT(directory, &st) != 0) {
    fprintf(stderr, "Error: '%s' does not exist\n", directory);
    return 1;
  }
#ifdef _WIN32
  if (!(st.st_mode & _S_IFDIR)) {
#else
  if (!S_ISDIR(st.st_mode)) {
#endif
    fprintf(stderr, "Error: '%s' is not a directory\n", directory);
    return 1;
  }

  // Resolve to absolute path
  char root[4096];
#ifdef _WIN32
  if (!_fullpath(root, directory, sizeof(root))) {
#else
  if (!realpath(directory, root)) {
#endif
    fprintf(stderr, "Error: could not resolve path '%s'\n", directory);
    return 1;
  }

  isr_set_server_root(root);

  net_sock_t server_sock = net_listen(port);
  if (server_sock == NET_INVALID_SOCKET) {
    fprintf(stderr, "Failed to listen on port %s\n", port);
    return 1;
  }
  printf("Serving '%s' on port %s\n", root, port);

  // Accept loop (single-threaded)
  for (;;) {
    net_sock_t client = net_accept(server_sock);
    if (client == NET_INVALID_SOCKET) {
      fprintf(stderr, "Failed to accept connection\n");
      continue;
    }
    printf("Client connected.\n");
    int result = isr_receive_command(client);
    if (result == 0) {
      printf("Request completed successfully.\n");
    } else {
      printf("Request failed.\n");
    }
    net_close(client);
  }

  net_close(server_sock);
  return 0;
}

static int cmd_send(int argc, char* argv[]) {
  const char* hostport = NULL;
  const char* local_file = NULL;
  const char* remote_path = NULL;
  uint32_t block_size = 0;
  uint8_t flags = 0;

  // Parse args
  if (argc < 3) { usage(); return 1; }
  hostport = argv[0];
  local_file = argv[1];
  remote_path = argv[2];
  for (int i = 3; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      block_size = (uint32_t)atoi(argv[++i]) * 1024;
      flags |= ISR_FLAG_USE_COMPRESSION;
    } else if (strcmp(argv[i], "-o") == 0) {
      flags |= ISR_FLAG_OVERWRITE;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      usage();
      return 1;
    }
  }

  // Parse host:port
  char host[256], port[16];
  if (parse_host_port(hostport, host, sizeof(host), port, sizeof(port)) == -1) {
    fprintf(stderr, "Invalid host:port format: %s\n", hostport);
    return 1;
  }

  // Open and stat local file
  ISR_STRUCT_STAT st;
  if (ISR_STAT(local_file, &st) != 0) {
    fprintf(stderr, "Error: cannot stat '%s'\n", local_file);
    return 1;
  }
  uint64_t file_size = (uint64_t)st.st_size;

#ifdef _WIN32
  int fd = _open(local_file, _O_RDONLY | _O_BINARY);
#else
  int fd = open(local_file, O_RDONLY);
#endif
  if (fd < 0) {
    fprintf(stderr, "Error: cannot open '%s'\n", local_file);
    return 1;
  }

  // Connect
  net_sock_t sock = net_connect(host, port);
  if (sock == NET_INVALID_SOCKET) {
    fprintf(stderr, "Failed to connect to %s:%s\n", host, port);
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
    return 1;
  }

  // Send command
  if (isr_transmit_send_command(sock, remote_path, flags,
                                 block_size, file_size) == -1) {
    fprintf(stderr, "Failed to send command\n");
    goto send_cleanup;
  }

  // Read server response
  isr_response_t response;
  if (recv_exact(sock, &response, sizeof(response)) == -1) {
    fprintf(stderr, "Failed to read server response\n");
    goto send_cleanup;
  }

  if (response.response_type == ISR_RESULT_OTHER_FAILURE) {
    fprintf(stderr, "Server error: %s\n", response.response_data);
    goto send_cleanup;
  }

  printf("Server accepted (%s). Sending %lu bytes...\n",
         response.response_type == 0x00 ? "creating" : "overwriting",
         (unsigned long)file_size);

  // Stream file data
  int64_t checksum;
  if (flags & ISR_FLAG_USE_COMPRESSION) {
    checksum = isr_transmit_file_compressed(sock, fd, block_size);
  } else {
    checksum = isr_transmit_file_uncompressed(sock, fd);
  }

  if (checksum == -1) {
    fprintf(stderr, "Failed to transmit file data\n");
    goto send_cleanup;
  }

  // Read final result
  isr_response_t result;
  if (recv_exact(sock, &result, sizeof(result)) == -1) {
    fprintf(stderr, "Failed to read server result\n");
    goto send_cleanup;
  }

  if (result.response_type == ISR_RESULT_OK) {
    printf("Send successful.\n");
  } else if (result.response_type == ISR_RESULT_CHECKSUM_BAD) {
    fprintf(stderr, "Checksum mismatch!\n");
  } else {
    fprintf(stderr, "Server error: %s\n", result.response_data);
  }

#ifdef _WIN32
  _close(fd);
#else
  close(fd);
#endif
  net_close(sock);
  return (result.response_type == ISR_RESULT_OK) ? 0 : 1;

send_cleanup:
#ifdef _WIN32
  _close(fd);
#else
  close(fd);
#endif
  net_close(sock);
  return 1;
}

static int cmd_recv(int argc, char* argv[]) {
  const char* hostport = NULL;
  const char* remote_path = NULL;
  const char* local_path = NULL;
  uint32_t block_size = 0;
  uint8_t flags = 0;

  // Parse args
  if (argc < 2) { usage(); return 1; }
  hostport = argv[0];
  remote_path = argv[1];

  // Parse optional args
  int argi = 2;
  // Check if next arg is local_path (not a flag)
  if (argi < argc && argv[argi][0] != '-') {
    local_path = argv[argi++];
  }
  for (int i = argi; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      block_size = (uint32_t)atoi(argv[++i]) * 1024;
      flags |= ISR_FLAG_USE_COMPRESSION;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      usage();
      return 1;
    }
  }

  // Parse host:port
  char host[256], port[16];
  if (parse_host_port(hostport, host, sizeof(host), port, sizeof(port)) == -1) {
    fprintf(stderr, "Invalid host:port format: %s\n", hostport);
    return 1;
  }

  // Connect
  net_sock_t sock = net_connect(host, port);
  if (sock == NET_INVALID_SOCKET) {
    fprintf(stderr, "Failed to connect to %s:%s\n", host, port);
    return 1;
  }

  // Send recv command
  if (isr_transmit_recv_command(sock, remote_path, flags, block_size) == -1) {
    fprintf(stderr, "Failed to send receive command\n");
    net_close(sock);
    return 1;
  }

  // Read server confirmation
  isr_recv_confirmation_t conf;
  if (recv_exact(sock, &conf, sizeof(conf)) == -1) {
    fprintf(stderr, "Failed to read server response\n");
    net_close(sock);
    return 1;
  }

  uint64_t effective_length = be64toh(conf.effective_length);

  if (conf.response_type == 0x03) {
    fprintf(stderr, "Path not found on server.\n");
    net_close(sock);
    return 1;
  }

  if (conf.response_type == ISR_RESULT_OTHER_FAILURE) {
    fprintf(stderr, "Server error.\n");
    net_close(sock);
    return 1;
  }

  if (conf.response_type == 0x01) {
    // Directory listing
    printf("Directory listing for '%s':\n", remote_path);

    // Send go
    uint8_t go = ISR_RECV_GO;
    if (net_send(sock, &go, 1) == -1) {
      fprintf(stderr, "Failed to send confirmation\n");
      net_close(sock);
      return 1;
    }

    int result = isr_receive_directory_listing(sock, effective_length);
    net_close(sock);
    return result == 0 ? 0 : 1;

  } else if (conf.response_type == 0x00) {
    // File data
    printf("Receiving file (%lu bytes)...\n", (unsigned long)effective_length);

    // Determine local path
    const char* out_path = local_path;
    if (!out_path) {
      // Use basename of remote_path
      const char* slash = strrchr(remote_path, '/');
      out_path = slash ? slash + 1 : remote_path;
    }

    // Send go
    uint8_t go = ISR_RECV_GO;
    if (net_send(sock, &go, 1) == -1) {
      fprintf(stderr, "Failed to send confirmation\n");
      net_close(sock);
      return 1;
    }

    // Open output file
#ifdef _WIN32
    int fd = _open(out_path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
    if (fd < 0) {
      fprintf(stderr, "Error: cannot open '%s' for writing\n", out_path);
      net_close(sock);
      return 1;
    }

    int64_t result;
    if (flags & ISR_FLAG_USE_COMPRESSION) {
      result = isr_receive_file_compressed(sock, fd, block_size,
                                            effective_length);
    } else {
      result = isr_receive_file_uncompressed(sock, fd, effective_length);
    }

#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
    net_close(sock);

    if (result == -1) {
      fprintf(stderr, "Failed to receive file.\n");
      return 1;
    } else if (result == -2) {
      fprintf(stderr, "Checksum mismatch!\n");
      return 1;
    }

    printf("Received '%s' successfully.\n", out_path);
    return 0;

  } else {
    fprintf(stderr, "Unexpected response type: 0x%02x\n", conf.response_type);
    net_close(sock);
    return 1;
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    usage();
    return 1;
  }

  net_init();

  int result;
  if (strcmp(argv[1], "serve") == 0) {
    result = cmd_serve(argc - 2, argv + 2);
  } else if (strcmp(argv[1], "send") == 0) {
    result = cmd_send(argc - 2, argv + 2);
  } else if (strcmp(argv[1], "recv") == 0) {
    result = cmd_recv(argc - 2, argv + 2);
  } else {
    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    usage();
    result = 1;
  }

  net_cleanup();
  return result;
}
