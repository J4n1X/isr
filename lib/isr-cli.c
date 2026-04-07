#include "isr-cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

// Connect to server and send recv command, reading back the confirmation.
// Returns socket on success (caller must close), NET_INVALID_SOCKET on failure.
static net_sock_t recv_connect(const char* host, const char* port,
                               const char* remote_path, uint8_t flags,
                               uint32_t block_size,
                               isr_recv_confirmation_t* conf) {
  net_sock_t sock = net_connect(host, port);
  if (sock == NET_INVALID_SOCKET) {
    fprintf(stderr, "Failed to connect to %s:%s\n", host, port);
    return NET_INVALID_SOCKET;
  }

  if (isr_transmit_recv_command(sock, remote_path, flags, block_size) == -1) {
    fprintf(stderr, "Failed to send receive command\n");
    net_close(sock);
    return NET_INVALID_SOCKET;
  }

  if (net_recv_exact(sock, conf, sizeof(*conf)) == -1) {
    fprintf(stderr, "Failed to read server response\n");
    net_close(sock);
    return NET_INVALID_SOCKET;
  }

  return sock;
}

static int send_go(net_sock_t sock) {
  uint8_t go = ISR_RECV_GO;
  if (net_send(sock, &go, 1) == -1) {
    fprintf(stderr, "Failed to send confirmation\n");
    return -1;
  }
  return 0;
}

// Receive file data to an open fd, handling compression.
// Returns 0 on success, -1 on I/O error, -2 on checksum mismatch.
static int receive_file_data(net_sock_t sock, int fd, uint8_t flags,
                             uint32_t block_size, uint64_t effective_length) {
  int64_t result;
  if (flags & ISR_FLAG_USE_COMPRESSION) {
    result = isr_receive_file_compressed(sock, fd, block_size, effective_length);
  } else {
    result = isr_receive_file_uncompressed(sock, fd, effective_length);
  }
  return (result >= 0) ? 0 : (int)result;
}

int isr_cli_send_file(const char* host, const char* port,
                      const char* remote_path, const char* local_file,
                      uint8_t flags, uint32_t block_size) {
  uint64_t file_size = 0;
  if (isr_stat(local_file, NULL, &file_size) != 0) {
    fprintf(stderr, "Error: cannot stat '%s'\n", local_file);
    return -1;
  }

  int fd = isr_open_read(local_file);
  if (fd < 0) {
    fprintf(stderr, "Error: cannot open '%s'\n", local_file);
    return -1;
  }

  net_sock_t sock = net_connect(host, port);
  if (sock == NET_INVALID_SOCKET) {
    fprintf(stderr, "Failed to connect to %s:%s\n", host, port);
    isr_close(fd);
    return -1;
  }

  int ret = -1;

  if (isr_transmit_send_command(sock, remote_path, flags,
                                block_size, file_size) == -1) {
    fprintf(stderr, "Failed to send command\n");
    goto cleanup;
  }

  isr_response_t response;
  if (net_recv_exact(sock, &response, sizeof(response)) == -1) {
    fprintf(stderr, "Failed to read server response\n");
    goto cleanup;
  }

  if (response.response_type == ISR_RESULT_OTHER_FAILURE) {
    fprintf(stderr, "Server error: %s\n", response.response_data);
    goto cleanup;
  }

  printf("Server accepted (%s). Sending %lu bytes...\n",
         response.response_type == ISR_SEND_RESPONSE_CREATING
             ? "creating" : "overwriting",
         (unsigned long)file_size);

  int64_t checksum;
  if (flags & ISR_FLAG_USE_COMPRESSION) {
    checksum = isr_transmit_file_compressed(sock, fd, block_size);
  } else {
    checksum = isr_transmit_file_uncompressed(sock, fd);
  }

  if (checksum == -1) {
    fprintf(stderr, "Failed to transmit file data\n");
    goto cleanup;
  }

  isr_response_t result;
  if (net_recv_exact(sock, &result, sizeof(result)) == -1) {
    fprintf(stderr, "Failed to read server result\n");
    goto cleanup;
  }

  if (result.response_type == ISR_RESULT_OK) {
    printf("Send successful.\n");
    ret = 0;
  } else if (result.response_type == ISR_RESULT_CHECKSUM_BAD) {
    fprintf(stderr, "Checksum mismatch!\n");
  } else {
    fprintf(stderr, "Server error: %s\n", result.response_data);
  }

cleanup:
  isr_close(fd);
  net_close(sock);
  return ret;
}

int isr_cli_send_mkdir(const char* host, const char* port,
                       const char* remote_path) {
  net_sock_t sock = net_connect(host, port);
  if (sock == NET_INVALID_SOCKET) {
    fprintf(stderr, "Failed to connect to %s:%s\n", host, port);
    return -1;
  }

  if (isr_transmit_send_command(sock, remote_path,
                                ISR_FLAG_CREATE_DIRECTORY, 0, 0) == -1) {
    fprintf(stderr, "Failed to send mkdir command\n");
    net_close(sock);
    return -1;
  }

  isr_response_t response;
  if (net_recv_exact(sock, &response, sizeof(response)) == -1) {
    fprintf(stderr, "Failed to read server response\n");
    net_close(sock);
    return -1;
  }

  net_close(sock);

  if (response.response_type != ISR_RESULT_OK) {
    fprintf(stderr, "Server error creating directory '%s': %s\n",
            remote_path, response.response_data);
    return -1;
  }

  return 0;
}

int isr_cli_send_directory(const char* host, const char* port,
                           const char* remote_path, const char* local_dir,
                           uint8_t flags, uint32_t block_size) {
  printf("Uploading directory: %s -> %s\n", local_dir, remote_path);

  // Create the remote directory first
  if (isr_cli_send_mkdir(host, port, remote_path) != 0) {
    fprintf(stderr, "Failed to create remote directory: %s\n", remote_path);
    return -1;
  }

  int result = 0;

#ifdef _WIN32
  char pattern[ISR_PATH_MAX];
  snprintf(pattern, sizeof(pattern), "%s\\*", local_dir);

  WIN32_FIND_DATAA fd;
  HANDLE hFind = FindFirstFileA(pattern, &fd);
  if (hFind == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "Error: cannot open directory '%s'\n", local_dir);
    return -1;
  }

  do {
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
      continue;

    char local_entry[ISR_PATH_MAX];
    char remote_entry[ISR_PATH_MAX];
    snprintf(local_entry, sizeof(local_entry), "%s/%s", local_dir, fd.cFileName);
    snprintf(remote_entry, sizeof(remote_entry), "%s/%s",
             remote_path, fd.cFileName);

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (isr_cli_send_directory(host, port, remote_entry, local_entry,
                                 flags, block_size) != 0) {
        fprintf(stderr, "Failed to upload directory: %s\n", local_entry);
        result = -1;
      }
    } else {
      printf("  Uploading: %s -> %s\n", local_entry, remote_entry);
      if (isr_cli_send_file(host, port, remote_entry, local_entry,
                            flags, block_size) != 0) {
        fprintf(stderr, "Failed to upload file: %s\n", local_entry);
        result = -1;
      }
    }
  } while (FindNextFileA(hFind, &fd));

  FindClose(hFind);
#else
  DIR* dir = opendir(local_dir);
  if (!dir) {
    fprintf(stderr, "Error: cannot open directory '%s'\n", local_dir);
    return -1;
  }

  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    char local_entry[ISR_PATH_MAX];
    char remote_entry[ISR_PATH_MAX];
    snprintf(local_entry, sizeof(local_entry), "%s/%s",
             local_dir, entry->d_name);
    snprintf(remote_entry, sizeof(remote_entry), "%s/%s",
             remote_path, entry->d_name);

    int is_dir = 0;
    if (isr_stat(local_entry, &is_dir, NULL) != 0) {
      fprintf(stderr, "Error: cannot stat '%s'\n", local_entry);
      result = -1;
      continue;
    }

    if (is_dir) {
      if (isr_cli_send_directory(host, port, remote_entry, local_entry,
                                 flags, block_size) != 0) {
        fprintf(stderr, "Failed to upload directory: %s\n", local_entry);
        result = -1;
      }
    } else {
      printf("  Uploading: %s -> %s\n", local_entry, remote_entry);
      if (isr_cli_send_file(host, port, remote_entry, local_entry,
                            flags, block_size) != 0) {
        fprintf(stderr, "Failed to upload file: %s\n", local_entry);
        result = -1;
      }
    }
  }

  closedir(dir);
#endif

  if (result == 0) {
    printf("Directory upload complete: %s\n", remote_path);
  }
  return result;
}

int isr_cli_recv(const char* host, const char* port,
                 const char* remote_path, const char* local_dir,
                 uint8_t flags, uint32_t block_size) {
  isr_recv_confirmation_t conf;
  net_sock_t sock = recv_connect(host, port, remote_path, flags,
                                 block_size, &conf);
  if (sock == NET_INVALID_SOCKET) return -1;

  uint64_t effective_length = be64toh(conf.effective_length);

  if (conf.response_type == ISR_RECV_TYPE_NOT_FOUND) {
    fprintf(stderr, "Path not found on server.\n");
    net_close(sock);
    return -1;
  }

  if (conf.response_type == ISR_RESULT_OTHER_FAILURE) {
    fprintf(stderr, "Server error.\n");
    net_close(sock);
    return -1;
  }

  if (conf.response_type == ISR_RECV_TYPE_DIRECTORY) {
    printf("Directory listing for '%s':\n", remote_path);
    if (send_go(sock) == -1) { net_close(sock); return -1; }
    int result = isr_receive_directory_listing(sock, effective_length);
    net_close(sock);
    return result == 0 ? 0 : -1;
  }

  if (conf.response_type == ISR_RECV_TYPE_FILE) {
    printf("Receiving file (%lu bytes)...\n", (unsigned long)effective_length);
    if (send_go(sock) == -1) { net_close(sock); return -1; }

    const char* basename = strrchr(remote_path, '/');
    basename = basename ? basename + 1 : remote_path;

    char out_path[ISR_PATH_MAX];
    snprintf(out_path, sizeof(out_path), "%s/%s", local_dir, basename);

    isr_mkdir_recursive(local_dir);

    int fd = isr_open_write(out_path);
    if (fd < 0) {
      fprintf(stderr, "Error: cannot open '%s' for writing\n", out_path);
      net_close(sock);
      return -1;
    }

    int result = receive_file_data(sock, fd, flags, block_size,
                                   effective_length);
    isr_close(fd);
    net_close(sock);

    if (result == -1) {
      fprintf(stderr, "Failed to receive file.\n");
      return -1;
    } else if (result == -2) {
      fprintf(stderr, "Checksum mismatch!\n");
      return -1;
    }

    printf("Received '%s' successfully.\n", out_path);
    return 0;
  }

  fprintf(stderr, "Unexpected response type: 0x%02x\n", conf.response_type);
  net_close(sock);
  return -1;
}

int isr_cli_download_file(const char* host, const char* port,
                          const char* remote_path, const char* local_path,
                          uint8_t flags, uint32_t block_size) {
  isr_recv_confirmation_t conf;
  net_sock_t sock = recv_connect(host, port, remote_path, flags,
                                 block_size, &conf);
  if (sock == NET_INVALID_SOCKET) return -1;

  uint64_t effective_length = be64toh(conf.effective_length);

  if (conf.response_type == ISR_RECV_TYPE_NOT_FOUND) {
    fprintf(stderr, "  File not found: %s\n", remote_path);
    net_close(sock);
    return -1;
  }

  if (conf.response_type != ISR_RECV_TYPE_FILE) {
    fprintf(stderr, "  Expected file but got type 0x%02x for %s\n",
            conf.response_type, remote_path);
    net_close(sock);
    return -1;
  }

  if (send_go(sock) == -1) { net_close(sock); return -1; }

  // Create parent directories
  char parent[ISR_PATH_MAX];
  if (isr_parent_dir(local_path, parent, sizeof(parent)) == 0) {
    isr_mkdir_recursive(parent);
  }

  int fd = isr_open_write(local_path);
  if (fd < 0) {
    fprintf(stderr, "Error: cannot open '%s' for writing\n", local_path);
    net_close(sock);
    return -1;
  }

  printf("  Downloading: %s -> %s (%lu bytes)\n",
         remote_path, local_path, (unsigned long)effective_length);

  int result = receive_file_data(sock, fd, flags, block_size,
                                 effective_length);
  isr_close(fd);
  net_close(sock);

  if (result == -1) {
    fprintf(stderr, "  Failed to receive file.\n");
    return -1;
  } else if (result == -2) {
    fprintf(stderr, "  Checksum mismatch!\n");
    return -1;
  }

  return 0;
}

isr_cli_dir_entry_t* isr_cli_parse_directory_listing(const uint8_t* buf,
                                                     size_t buf_len,
                                                     int* entry_count) {
  // Count entries first
  size_t offset = 0;
  int count = 0;
  while (offset < buf_len) {
    if (offset + 11 > buf_len) break;
    offset++;     // is_dir byte
    offset += 8;  // size
    uint16_t name_len_be;
    memcpy(&name_len_be, &buf[offset], 2);
    uint16_t name_len = ntohs(name_len_be);
    offset += 2;
    if (offset + name_len > buf_len) break;
    offset += name_len;
    count++;
  }

  if (count == 0) {
    *entry_count = 0;
    return NULL;
  }

  isr_cli_dir_entry_t* entries = malloc(count * sizeof(isr_cli_dir_entry_t));
  if (!entries) {
    *entry_count = 0;
    return NULL;
  }

  // Parse entries
  offset = 0;
  int idx = 0;
  while (offset < buf_len && idx < count) {
    if (offset + 11 > buf_len) break;

    entries[idx].is_directory = buf[offset++];

    uint64_t size_be;
    memcpy(&size_be, &buf[offset], 8);
    entries[idx].size = be64toh(size_be);
    offset += 8;

    uint16_t name_len_be;
    memcpy(&name_len_be, &buf[offset], 2);
    uint16_t name_len = ntohs(name_len_be);
    offset += 2;

    if (offset + name_len > buf_len) break;

    entries[idx].name = malloc(name_len + 1);
    if (!entries[idx].name) {
      isr_cli_free_entries(entries, idx);
      *entry_count = 0;
      return NULL;
    }
    memcpy(entries[idx].name, &buf[offset], name_len);
    entries[idx].name[name_len] = '\0';
    offset += name_len;
    idx++;
  }

  *entry_count = idx;
  return entries;
}

void isr_cli_free_entries(isr_cli_dir_entry_t* entries, int count) {
  for (int i = 0; i < count; i++) {
    free(entries[i].name);
  }
  free(entries);
}

// Receive a directory listing buffer from the network and verify its checksum.
// Returns the allocated buffer on success (caller must free), NULL on failure.
// Sets *out_len to the buffer length on success.
static uint8_t* recv_verified_listing(net_sock_t sock, uint64_t effective_length) {
  uint8_t* buf = malloc((size_t)effective_length);
  if (!buf) return NULL;

  if (net_recv_exact(sock, buf, (size_t)effective_length) == -1) {
    free(buf);
    return NULL;
  }

  uint64_t checksum = _isr_count_byte_values(buf, (size_t)effective_length);
  uint64_t wire_checksum_be;
  if (net_recv_exact(sock, &wire_checksum_be, sizeof(wire_checksum_be)) == -1) {
    free(buf);
    return NULL;
  }
  uint64_t wire_checksum = be64toh(wire_checksum_be);
  if (wire_checksum != checksum) {
    fprintf(stderr, "Checksum mismatch for directory listing\n");
    free(buf);
    return NULL;
  }

  return buf;
}

int isr_cli_download_directory(const char* host, const char* port,
                               const char* remote_path, const char* local_dir,
                               uint8_t flags, uint32_t block_size) {
  printf("Downloading directory: %s -> %s\n", remote_path, local_dir);

  isr_recv_confirmation_t conf;
  net_sock_t sock = recv_connect(host, port, remote_path, flags,
                                 block_size, &conf);
  if (sock == NET_INVALID_SOCKET) return -1;

  uint64_t effective_length = be64toh(conf.effective_length);

  if (conf.response_type == ISR_RECV_TYPE_NOT_FOUND) {
    fprintf(stderr, "Directory not found on server: %s\n", remote_path);
    net_close(sock);
    return -1;
  }

  if (conf.response_type == ISR_RESULT_OTHER_FAILURE) {
    fprintf(stderr, "Server error for path: %s\n", remote_path);
    net_close(sock);
    return -1;
  }

  if (conf.response_type == ISR_RECV_TYPE_FILE) {
    // It's actually a file, just download it
    net_close(sock);

    const char* basename = strrchr(remote_path, '/');
    basename = basename ? basename + 1 : remote_path;

    char local_file_path[ISR_PATH_MAX];
    snprintf(local_file_path, sizeof(local_file_path), "%s/%s",
             local_dir, basename);

    return isr_cli_download_file(host, port, remote_path, local_file_path,
                                 flags, block_size);
  }

  if (conf.response_type != ISR_RECV_TYPE_DIRECTORY) {
    fprintf(stderr, "Unexpected response type: 0x%02x\n", conf.response_type);
    net_close(sock);
    return -1;
  }

  if (send_go(sock) == -1) { net_close(sock); return -1; }

  uint8_t* buf = recv_verified_listing(sock, effective_length);
  net_close(sock);
  if (!buf) return -1;

  // Parse the directory listing
  int entry_count = 0;
  isr_cli_dir_entry_t* entries = isr_cli_parse_directory_listing(
      buf, effective_length, &entry_count);
  free(buf);

  if (!entries && entry_count != 0) {
    fprintf(stderr, "Failed to parse directory listing\n");
    return -1;
  }

  isr_mkdir_recursive(local_dir);

  int result = 0;

  // Process each entry
  for (int i = 0; i < entry_count; i++) {
    char remote_entry_path[ISR_PATH_MAX];
    char local_entry_path[ISR_PATH_MAX];

    snprintf(remote_entry_path, sizeof(remote_entry_path), "%s/%s",
             remote_path, entries[i].name);
    snprintf(local_entry_path, sizeof(local_entry_path), "%s/%s",
             local_dir, entries[i].name);

    if (entries[i].is_directory) {
      if (isr_cli_download_directory(host, port, remote_entry_path,
                                     local_entry_path, flags,
                                     block_size) != 0) {
        fprintf(stderr, "Failed to download directory: %s\n",
                remote_entry_path);
        result = -1;
      }
    } else {
      if (isr_cli_download_file(host, port, remote_entry_path,
                                local_entry_path, flags, block_size) != 0) {
        fprintf(stderr, "Failed to download file: %s\n", remote_entry_path);
        result = -1;
      }
    }
  }

  isr_cli_free_entries(entries, entry_count);

  if (result == 0) {
    printf("Directory download complete: %s\n", remote_path);
  }
  return result;
}
