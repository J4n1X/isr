#define NET_IMPLEMENTATION
#include "lib/mininet.h"
#include "lib/isr.h"
#include "lib/isr-cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define DEFAULT_PORT "42069"

static void usage(void) {
  fprintf(stderr,
    "Usage:\n"
    "  isr serve <directory> [-p port]\n"
    "  isr send <host:port> <local_path> <remote_path> [-c blocksize] [-o] [-r]\n"
    "  isr recv <host:port> <remote_path> [local_dir] [-c blocksize] [-r]\n"
    "\n"
    "Options:\n"
    "  -p port       Port to listen on (default: " DEFAULT_PORT ")\n"
    "  -c blocksize  Enable LZ4 compression with given block size in KB\n"
    "  -o            Overwrite existing files\n"
    "  -r            Recursive mode (upload/download entire directory trees)\n"
    "\n"
    "Send notes:\n"
    "  - Without -r: sends a single file\n"
    "  - With -r: uploads entire directory trees\n"
    "\n"
    "Recv notes:\n"
    "  - local_dir: Directory to save files (defaults to current directory)\n"
    "  - Without -r: directories show listings, files are downloaded\n"
    "  - With -r: entire directory trees are downloaded\n"
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

static void get_peer_str(net_sock_t sock, char* buf, size_t len) {
  struct sockaddr_storage ss;
  socklen_t ss_len = sizeof(ss);
  if (getpeername(sock, (struct sockaddr*)&ss, &ss_len) != 0) {
    snprintf(buf, len, "unknown");
    return;
  }
  char host[128], svc[16];
  if (getnameinfo((struct sockaddr*)&ss, ss_len,
                  host, sizeof(host), svc, sizeof(svc),
                  NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    snprintf(buf, len, "unknown");
    return;
  }
  if (ss.ss_family == AF_INET6) {
    snprintf(buf, len, "[%s]:%s", host, svc);
  } else {
    snprintf(buf, len, "%s:%s", host, svc);
  }
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

  // Strip trailing path separators (Windows _stat64 fails on paths like "..\")
  char dir_buf[4096];
  strncpy(dir_buf, directory, sizeof(dir_buf) - 1);
  dir_buf[sizeof(dir_buf) - 1] = '\0';
  size_t dlen = strlen(dir_buf);
  while (dlen > 1 && (dir_buf[dlen-1] == '/' || dir_buf[dlen-1] == '\\')) {
    dir_buf[--dlen] = '\0';
  }
  directory = dir_buf;

  // Validate directory
  int is_dir = 0;
  if (isr_stat(directory, &is_dir, NULL) != 0) {
    fprintf(stderr, "Error: '%s' does not exist\n", directory);
    return 1;
  }
  if (!is_dir) {
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

    char peer[256];
    get_peer_str(client, peer, sizeof(peer));

    isr_request_info_t info = {0};
    int result = isr_receive_command(client, &info);

    if (info.path[0] == '\0') {
      printf("[%s] connection error\n", peer);
    } else if (info.cmd == ISR_COMMAND_SEND && info.is_mkdir) {
      printf("[%s] MKDIR \"%s\" -> %s\n",
             peer, info.path, result == 0 ? "OK" : "FAILED");
    } else if (info.cmd == ISR_COMMAND_SEND) {
      printf("[%s] SEND \"%s\" (%lu bytes)%s%s -> %s\n",
             peer, info.path, (unsigned long)info.file_size,
             info.is_overwrite ? " [overwrite]" : "",
             info.is_compressed ? " [compressed]" : "",
             result == 0 ? "OK" : "FAILED");
    } else if (info.not_found) {
      printf("[%s] RECV \"%s\" -> NOT FOUND\n", peer, info.path);
    } else if (info.is_dir) {
      printf("[%s] RECV \"%s\" [dir] -> %s\n",
             peer, info.path, result == 0 ? "OK" : "FAILED");
    } else {
      printf("[%s] RECV \"%s\" (%lu bytes)%s -> %s\n",
             peer, info.path, (unsigned long)info.file_size,
             info.is_compressed ? " [compressed]" : "",
             result == 0 ? "OK" : "FAILED");
    }

    net_close(client);
  }

  net_close(server_sock);
  return 0;
}

static int cmd_send(int argc, char* argv[]) {
  const char* hostport = NULL;
  const char* local_path = NULL;
  const char* remote_path = NULL;
  uint32_t block_size = 0;
  uint8_t flags = 0;
  int recursive = 0;

  // Parse args
  if (argc < 3) { usage(); return 1; }
  hostport = argv[0];
  local_path = argv[1];
  remote_path = argv[2];
  for (int i = 3; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      block_size = (uint32_t)atoi(argv[++i]) * 1024;
      flags |= ISR_FLAG_USE_COMPRESSION;
    } else if (strcmp(argv[i], "-o") == 0) {
      flags |= ISR_FLAG_OVERWRITE;
    } else if (strcmp(argv[i], "-r") == 0) {
      recursive = 1;
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

  // Check if local_path is a directory
  int is_dir = 0;
  if (isr_stat(local_path, &is_dir, NULL) != 0) {
    fprintf(stderr, "Error: '%s' does not exist\n", local_path);
    return 1;
  }

  if (is_dir) {
    if (!recursive) {
      fprintf(stderr, "Error: '%s' is a directory (use -r for recursive send)\n",
              local_path);
      return 1;
    }
    return isr_cli_send_directory(host, port, remote_path, local_path,
                                  flags, block_size);
  }

  return isr_cli_send_file(host, port, remote_path, local_path,
                            flags, block_size);
}

static int cmd_recv(int argc, char* argv[]) {
  const char* hostport = NULL;
  const char* remote_path = NULL;
  const char* local_path = NULL;
  uint32_t block_size = 0;
  uint8_t flags = 0;
  int recursive = 0;

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
    } else if (strcmp(argv[i], "-r") == 0) {
      recursive = 1;
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

  // Determine local directory (default to current directory)
  const char* local_dir = local_path ? local_path : ".";

  if (recursive) {
    return isr_cli_download_directory(host, port, remote_path, local_dir,
                                      flags, block_size);
  }

  return isr_cli_recv(host, port, remote_path, local_dir, flags, block_size);
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
