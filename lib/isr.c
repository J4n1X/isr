#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef _WIN32
  #include <io.h>
  #include <direct.h>
  #include <windows.h>
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <dirent.h>
  #include <fcntl.h>
  #include <libgen.h>
#endif

#include "isr.h"
#include "lz4.h"

// Server root directory (set before accepting connections)
static char isr_server_root_path[4096] = {0};

void isr_set_server_root(const char* root) {
  strncpy(isr_server_root_path, root, sizeof(isr_server_root_path) - 1);
  isr_server_root_path[sizeof(isr_server_root_path) - 1] = '\0';
}

// Read exactly len bytes from socket. Returns 0 on success, -1 on error/disconnect.
static int net_recv_exact(net_sock_t sock, void* buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    int r = net_recv(sock, (char*)buf + total, len - total);
    if (r <= 0) return -1;
    total += (size_t)r;
  }
  return 0;
}

// Send exactly len bytes. Returns 0 on success, -1 on error.
static int net_send_exact(net_sock_t sock, const void* buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    int r = net_send(sock, (const char*)buf + total, len - total);
    if (r <= 0) return -1;
    total += (size_t)r;
  }
  return 0;
}

// Validate that a path is safe (no traversal, no absolute paths).
static int isr_validate_path(const char* path) {
  if (!path || path[0] == '\0') return -1;
  if (path[0] == '/' || path[0] == '\\') return -1;
#ifdef _WIN32
  // Reject drive letters like C:
  if (strlen(path) >= 2 && path[1] == ':') return -1;
#endif
  // Check for .. components
  const char* p = path;
  while (*p) {
    if (p[0] == '.' && p[1] == '.') {
      // Check that it's a proper component (at start, after /, or before / or end)
      if ((p == path || p[-1] == '/' || p[-1] == '\\') &&
          (p[2] == '\0' || p[2] == '/' || p[2] == '\\')) {
        return -1;
      }
    }
    p++;
  }
  return 0;
}

// Resolve a relative path against the server root. Returns 0 on success.
static int isr_resolve_path(const char* root, const char* relative,
                             char* out, size_t out_size) {
  int n = snprintf(out, out_size, "%s/%s", root, relative);
  if (n < 0 || (size_t)n >= out_size) return -1;

#ifdef _WIN32
  char resolved[MAX_PATH];
  if (!_fullpath(resolved, out, MAX_PATH)) return -1;
#else
  char resolved[4096];
  // realpath may fail if the file doesn't exist yet, so we resolve
  // the parent directory instead
  char tmp[4096];
  strncpy(tmp, out, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

  // Find the last slash to get the parent directory
  char* last_slash = strrchr(tmp, '/');
  if (last_slash && last_slash != tmp) {
    char basename_buf[4096];
    strncpy(basename_buf, last_slash + 1, sizeof(basename_buf) - 1);
    basename_buf[sizeof(basename_buf) - 1] = '\0';
    *last_slash = '\0';

    char* rp = realpath(tmp, resolved);
    if (!rp) {
      // Parent doesn't exist yet, that's OK for send with mkdir
      // Just do a basic prefix check on the unresolved path
      strncpy(resolved, out, sizeof(resolved) - 1);
      resolved[sizeof(resolved) - 1] = '\0';
    } else {
      // Re-append the basename
      strncat(resolved, "/", sizeof(resolved) - strlen(resolved) - 1);
      strncat(resolved, basename_buf, sizeof(resolved) - strlen(resolved) - 1);
    }
  } else {
    // Path has no slash or is at root
    char* rp = realpath(out, resolved);
    if (!rp) {
      strncpy(resolved, out, sizeof(resolved) - 1);
      resolved[sizeof(resolved) - 1] = '\0';
    }
  }
#endif

  // Verify the resolved path starts with root
  size_t root_len = strlen(root);
  if (strncmp(resolved, root, root_len) != 0) return -1;

  strncpy(out, resolved, out_size - 1);
  out[out_size - 1] = '\0';
  return 0;
}

// Create directories recursively. Returns 0 on success.
static int isr_mkdir_recursive(const char* path) {
  char tmp[4096];
  strncpy(tmp, path, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

  for (char* p = tmp + 1; *p; p++) {
    if (*p == '/' || *p == '\\') {
      *p = '\0';
#ifdef _WIN32
      _mkdir(tmp);
#else
      mkdir(tmp, 0755);
#endif
      *p = '/';
    }
  }
#ifdef _WIN32
  return _mkdir(tmp) == 0 || errno == EEXIST ? 0 : -1;
#else
  return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

// Cross-platform file open for reading
static int isr_open_read(const char* path) {
#ifdef _WIN32
  return _open(path, _O_RDONLY | _O_BINARY);
#else
  return open(path, O_RDONLY);
#endif
}

// Cross-platform file open for writing (create/truncate)
static int isr_open_write(const char* path) {
#ifdef _WIN32
  return _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
  return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
}

// Cross-platform file close
static void isr_close(int fd) {
#ifdef _WIN32
  _close(fd);
#else
  close(fd);
#endif
}

// Cross-platform file stat. Returns 0 on success.
// Sets *is_dir to 1 if directory, 0 if file. Sets *size to file size.
static int isr_stat(const char* path, int* is_dir, uint64_t* size) {
#ifdef _WIN32
  struct _stat64 st;
  if (_stat64(path, &st) != 0) return -1;
  if (is_dir) *is_dir = (st.st_mode & _S_IFDIR) ? 1 : 0;
  if (size) *size = (uint64_t)st.st_size;
#else
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  if (is_dir) *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
  if (size) *size = (uint64_t)st.st_size;
#endif
  return 0;
}

int64_t isr_transmit_file_uncompressed(net_sock_t sock, int fd) { 
  uint8_t buffer[1024*64];
  int64_t checksum = 0;
  ssize_t bytes_read;
  while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
    checksum += _isr_count_byte_values(buffer, (uint32_t)bytes_read);
    ssize_t bytes_sent = 0;
    while(bytes_sent < bytes_read) {
      ssize_t result = net_send(sock, buffer + bytes_sent, bytes_read - bytes_sent);
      if (result == -1) {
        return -1; // Error occurred
      }
      bytes_sent += result;
    }
  }
  if (bytes_read == -1) {
    return -1; // Error occurred during read
  }
  // transmit the checksum as well
  uint64_t checksum_network_order = htobe64((uint64_t)checksum);
  if (net_send(sock, &checksum_network_order, 
               sizeof(checksum_network_order)) == -1) {
    return -1; // Error occurred during checksum send
  }
  return checksum;
}

int64_t isr_transmit_file_compressed(net_sock_t sock, int fd,
                                   uint32_t uncompressed_block_size) {
  int64_t checksum = 0;
  int max_compressed_size = LZ4_compressBound(uncompressed_block_size);
  uint8_t *buffer = malloc(uncompressed_block_size);
  if (!buffer) return -1; // Allocation failed
  uint8_t *compressed_buffer = malloc(max_compressed_size);
  if (!compressed_buffer) {
    free(buffer);
    return -1; // Allocation failed
  }
  ssize_t bytes_read;
  while ((bytes_read = read(fd, buffer, uncompressed_block_size)) > 0) {
    checksum += _isr_count_byte_values(buffer, (uint32_t)bytes_read);
    int compressed_size = LZ4_compress_default((const char*)buffer,
                                               (char*)compressed_buffer,
                                               (int)bytes_read,
                                               max_compressed_size);
    if (compressed_size <= 0) {
      free(buffer);
      free(compressed_buffer);
      return -1; // Compression failed
    }

    // Send the compressed size first 
    // (as a 4-byte integer in network byte order)
    uint32_t compressed_size_network_order = htonl((uint32_t)compressed_size);
    if (net_send(sock, &compressed_size_network_order, 
                 sizeof(compressed_size_network_order)) == -1) {
      free(buffer);
      free(compressed_buffer);
      return -1; // Error occurred during size send
    }

    ssize_t bytes_sent = 0;
    while(bytes_sent < compressed_size) {
      ssize_t result = net_send(sock, compressed_buffer + bytes_sent,
                                compressed_size - bytes_sent);
      if (result == -1) {
        free(buffer);
        free(compressed_buffer);
        return -1; // Error occurred
      }
      bytes_sent += result;
    }
  }

  // Transmit the checksum
  uint64_t checksum_network_order = htobe64((uint64_t)checksum);
  if (net_send(sock, &checksum_network_order,
               sizeof(checksum_network_order)) == -1) {
    checksum = -1; // Error occurred during checksum send
  }
  free(buffer);
  free(compressed_buffer);
  return checksum;
}

int isr_transmit_send_command(net_sock_t sock, const char* path,
                              uint8_t flags,
                              uint32_t uncompressed_block_size,
                              uint64_t effective_length) {
  // Send command type byte
  uint8_t cmd = ISR_COMMAND_SEND;
  if (net_send_exact(sock, &cmd, 1) == -1) return -1;

  // Build and send header (big-endian on wire)
  uint16_t path_len = (uint16_t)strlen(path);
  isr_send_command_header_t header;
  header.flags = flags;
  header.path_length = htons(path_len);
  header.uncompressed_block_size = htonl(uncompressed_block_size);
  header.effective_length = htobe64(effective_length);
  if (net_send_exact(sock, &header, sizeof(header)) == -1) return -1;

  // Send path (no null terminator)
  if (net_send_exact(sock, path, path_len) == -1) return -1;
  return 0;
}

int isr_transmit_send_response(net_sock_t sock, uint8_t response_type,
                               const char* response_data) {
  isr_response_t response;
  if (response_type == ISR_RESULT_OTHER_FAILURE && response_data) {
    response = isr_response_from_error(response_data);
  } else {
    response = isr_response_from_result(response_type);
  }
  if (net_send_exact(sock, &response, sizeof(response)) == -1) return -1;
  return 0;
}

int isr_transmit_send_result(net_sock_t sock, uint8_t response_type) {
  return isr_transmit_send_response(sock, response_type, NULL);
}

int isr_transmit_recv_command(net_sock_t sock, const char* path, uint8_t flags,
                              uint32_t uncompressed_block_size) {
  // Send command type byte
  uint8_t cmd = ISR_COMMAND_RECV;
  if (net_send_exact(sock, &cmd, 1) == -1) return -1;

  // Build and send header (big-endian on wire)
  uint16_t path_len = (uint16_t)strlen(path);
  isr_recv_command_header_t header;
  header.flags = flags;
  header.path_length = htons(path_len);
  header.uncompressed_block_size = htonl(uncompressed_block_size);
  if (net_send_exact(sock, &header, sizeof(header)) == -1) return -1;

  // Send path (no null terminator)
  if (net_send_exact(sock, path, path_len) == -1) return -1;
  return 0;
}

int isr_transmit_recv_directory_listing(net_sock_t sock, const char* path) {
  // Directory listing wire format per entry:
  //   uint8_t  type       (0 = file, 1 = directory)
  //   uint64_t size       (big-endian)
  //   uint16_t name_len   (big-endian)
  //   char     name[name_len]
  // Entire listing is followed by an 8-byte checksum.

#ifdef _WIN32
  char search_path[4096];
  snprintf(search_path, sizeof(search_path), "%s\\*", path);
  WIN32_FIND_DATAA ffd;
  HANDLE hFind = FindFirstFileA(search_path, &ffd);
  if (hFind == INVALID_HANDLE_VALUE) return -1;

  // First pass: compute total size and collect entries
  typedef struct { char name[260]; uint64_t size; uint8_t is_dir; } entry_t;
  entry_t entries[4096];
  int count = 0;
  uint64_t total_size = 0;

  do {
    if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
      continue;
    if (count >= 4096) break;
    strncpy(entries[count].name, ffd.cFileName, 259);
    entries[count].name[259] = '\0';
    entries[count].is_dir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    entries[count].size = ((uint64_t)ffd.nFileSizeHigh << 32) | ffd.nFileSizeLow;
    uint16_t nlen = (uint16_t)strlen(entries[count].name);
    total_size += 1 + 8 + 2 + nlen; // type + size + name_len + name
    count++;
  } while (FindNextFileA(hFind, &ffd));
  FindClose(hFind);
#else
  DIR* dir = opendir(path);
  if (!dir) return -1;

  typedef struct { char name[256]; uint64_t size; uint8_t is_dir; } entry_t;
  entry_t entries[4096];
  int count = 0;
  uint64_t total_size = 0;

  struct dirent* de;
  while ((de = readdir(dir)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    if (count >= 4096) break;
    snprintf(entries[count].name, sizeof(entries[count].name), "%s", de->d_name);

    // Stat the entry to get size and type
    char full_path[4096];
    snprintf(full_path, sizeof(full_path), "%s/%s", path, de->d_name);
    int is_dir = 0;
    uint64_t fsize = 0;
    isr_stat(full_path, &is_dir, &fsize);
    entries[count].is_dir = (uint8_t)is_dir;
    entries[count].size = fsize;

    uint16_t nlen = (uint16_t)strlen(entries[count].name);
    total_size += 1 + 8 + 2 + nlen;
    count++;
  }
  closedir(dir);
#endif

  // Stream entries and compute checksum
  uint64_t checksum = 0;
  for (int i = 0; i < count; i++) {
    uint8_t type = entries[i].is_dir;
    uint64_t size_be = htobe64(entries[i].size);
    uint16_t nlen = (uint16_t)strlen(entries[i].name);
    uint16_t nlen_be = htons(nlen);

    checksum += _isr_count_byte_values(&type, 1);
    if (net_send_exact(sock, &type, 1) == -1) return -1;

    checksum += _isr_count_byte_values((uint8_t*)&size_be, 8);
    if (net_send_exact(sock, &size_be, 8) == -1) return -1;

    checksum += _isr_count_byte_values((uint8_t*)&nlen_be, 2);
    if (net_send_exact(sock, &nlen_be, 2) == -1) return -1;

    checksum += _isr_count_byte_values((uint8_t*)entries[i].name, nlen);
    if (net_send_exact(sock, entries[i].name, nlen) == -1) return -1;
  }

  // Send checksum
  uint64_t checksum_be = htobe64(checksum);
  if (net_send_exact(sock, &checksum_be, sizeof(checksum_be)) == -1) return -1;
  return 0;
}

//
// Phase 3: Receive-side file functions
//

int64_t isr_receive_file_uncompressed(net_sock_t sock, int fd,
                                       uint64_t effective_length) {
  uint8_t buffer[1024 * 64];
  int64_t checksum = 0;
  uint64_t remaining = effective_length;

  while (remaining > 0) {
    size_t to_read = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
    if (net_recv_exact(sock, buffer, to_read) == -1) return -1;
    checksum += _isr_count_byte_values(buffer, (uint32_t)to_read);

    size_t written = 0;
    while (written < to_read) {
#ifdef _WIN32
      int w = _write(fd, buffer + written, (unsigned)(to_read - written));
#else
      ssize_t w = write(fd, buffer + written, to_read - written);
#endif
      if (w <= 0) return -1;
      written += (size_t)w;
    }
    remaining -= to_read;
  }

  // Read and verify checksum
  uint64_t wire_checksum_be;
  if (net_recv_exact(sock, &wire_checksum_be, sizeof(wire_checksum_be)) == -1)
    return -1;
  uint64_t wire_checksum = be64toh(wire_checksum_be);
  if (wire_checksum != (uint64_t)checksum) return -2;
  return checksum;
}

int64_t isr_receive_file_compressed(net_sock_t sock, int fd,
                                     uint32_t uncompressed_block_size,
                                     uint64_t effective_length) {
  int64_t checksum = 0;
  int max_compressed_size = LZ4_compressBound(uncompressed_block_size);
  uint8_t* compressed_buf = malloc(max_compressed_size);
  if (!compressed_buf) return -1;
  uint8_t* decompress_buf = malloc(uncompressed_block_size);
  if (!decompress_buf) { free(compressed_buf); return -1; }

  uint64_t remaining = effective_length;
  while (remaining > 0) {
    // Read compressed block size
    uint32_t block_size_be;
    if (net_recv_exact(sock, &block_size_be, sizeof(block_size_be)) == -1) {
      free(compressed_buf); free(decompress_buf); return -1;
    }
    uint32_t block_size = ntohl(block_size_be);

    // Read compressed block
    if (net_recv_exact(sock, compressed_buf, block_size) == -1) {
      free(compressed_buf); free(decompress_buf); return -1;
    }

    // Decompress
    int decompressed_size = LZ4_decompress_safe(
        (const char*)compressed_buf, (char*)decompress_buf,
        (int)block_size, (int)uncompressed_block_size);
    if (decompressed_size <= 0) {
      free(compressed_buf); free(decompress_buf); return -1;
    }

    checksum += _isr_count_byte_values(decompress_buf, (uint32_t)decompressed_size);

    // Write to file
    size_t written = 0;
    while (written < (size_t)decompressed_size) {
#ifdef _WIN32
      int w = _write(fd, decompress_buf + written,
                     (unsigned)(decompressed_size - written));
#else
      ssize_t w = write(fd, decompress_buf + written,
                        decompressed_size - written);
#endif
      if (w <= 0) { free(compressed_buf); free(decompress_buf); return -1; }
      written += (size_t)w;
    }
    remaining -= (uint64_t)decompressed_size;
  }

  free(compressed_buf);
  free(decompress_buf);

  // Read and verify checksum
  uint64_t wire_checksum_be;
  if (net_recv_exact(sock, &wire_checksum_be, sizeof(wire_checksum_be)) == -1)
    return -1;
  uint64_t wire_checksum = be64toh(wire_checksum_be);
  if (wire_checksum != (uint64_t)checksum) return -2;
  return checksum;
}

int isr_receive_directory_listing(net_sock_t sock, uint64_t effective_length) {
  // Read the entire listing into a buffer
  uint8_t* buf = malloc((size_t)effective_length);
  if (!buf) return -1;
  if (net_recv_exact(sock, buf, (size_t)effective_length) == -1) {
    free(buf); return -1;
  }

  // Verify checksum
  uint64_t checksum = _isr_count_byte_values(buf, (uint32_t)effective_length);
  uint64_t wire_checksum_be;
  if (net_recv_exact(sock, &wire_checksum_be, sizeof(wire_checksum_be)) == -1) {
    free(buf); return -1;
  }
  uint64_t wire_checksum = be64toh(wire_checksum_be);
  if (wire_checksum != checksum) { free(buf); return -1; }

  // Parse and print entries
  size_t offset = 0;
  while (offset < effective_length) {
    if (offset + 11 > effective_length) break; // need at least 1+8+2 bytes
    uint8_t type = buf[offset]; offset += 1;
    uint64_t size;
    memcpy(&size, buf + offset, 8); size = be64toh(size); offset += 8;
    uint16_t nlen;
    memcpy(&nlen, buf + offset, 2); nlen = ntohs(nlen); offset += 2;
    if (offset + nlen > effective_length) break;

    char name[4096];
    size_t copy_len = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
    memcpy(name, buf + offset, copy_len);
    name[copy_len] = '\0';
    offset += nlen;

    if (type == 1) {
      printf("  [DIR]  %s\n", name);
    } else {
      printf("  %10lu  %s\n", (unsigned long)size, name);
    }
  }

  free(buf);
  return 0;
}

//
// Phase 4: Server-side command handlers
//

int isr_receive_command(net_sock_t sock) {
  // Read command type byte
  uint8_t cmd;
  if (net_recv_exact(sock, &cmd, 1) == -1) return -1;

  if (cmd == ISR_COMMAND_SEND) {
    // Read send header
    isr_send_command_header_t header;
    if (net_recv_exact(sock, &header, sizeof(header)) == -1) return -1;

    // Convert from big-endian
    header.path_length = ntohs(header.path_length);
    header.uncompressed_block_size = ntohl(header.uncompressed_block_size);
    header.effective_length = be64toh(header.effective_length);

    // Read path
    char path[4096];
    if (header.path_length >= sizeof(path)) return -1;
    if (net_recv_exact(sock, path, header.path_length) == -1) return -1;
    path[header.path_length] = '\0';

    return isr_receive_send_command(sock, header, path);

  } else if (cmd == ISR_COMMAND_RECV) {
    // Read recv header
    isr_recv_command_header_t header;
    if (net_recv_exact(sock, &header, sizeof(header)) == -1) return -1;

    // Convert from big-endian
    header.path_length = ntohs(header.path_length);
    header.uncompressed_block_size = ntohl(header.uncompressed_block_size);

    // Read path
    char path[4096];
    if (header.path_length >= sizeof(path)) return -1;
    if (net_recv_exact(sock, path, header.path_length) == -1) return -1;
    path[header.path_length] = '\0';

    return isr_receive_recv_command(sock, header, path);

  } else {
    fprintf(stderr, "Unknown command type: 0x%02x\n", cmd);
    return -1;
  }
}

int isr_receive_send_command(net_sock_t sock,
                             isr_send_command_header_t header,
                             const char* path) {
  // Validate path
  if (isr_validate_path(path) == -1) {
    isr_transmit_send_response(sock, ISR_RESULT_OTHER_FAILURE,
                                "Invalid path");
    return -1;
  }

  // Resolve full path
  char full_path[4096];
  if (isr_resolve_path(isr_server_root_path, path,
                        full_path, sizeof(full_path)) == -1) {
    isr_transmit_send_response(sock, ISR_RESULT_OTHER_FAILURE,
                                "Path resolution failed");
    return -1;
  }

  // Handle create directory flag
  if (header.flags & ISR_FLAG_CREATE_DIRECTORY) {
    if (isr_mkdir_recursive(full_path) == -1) {
      isr_transmit_send_response(sock, ISR_RESULT_OTHER_FAILURE,
                                  "Failed to create directory");
      return -1;
    }
    isr_transmit_send_response(sock, ISR_RESULT_OK, NULL);
    return 0;
  }

  // Check if file exists
  int is_dir = 0;
  uint64_t existing_size = 0;
  int exists = (isr_stat(full_path, &is_dir, &existing_size) == 0);

  if (exists && is_dir) {
    isr_transmit_send_response(sock, ISR_RESULT_OTHER_FAILURE,
                                "Path is a directory");
    return -1;
  }

  if (exists && !(header.flags & ISR_FLAG_OVERWRITE)) {
    isr_transmit_send_response(sock, ISR_RESULT_OTHER_FAILURE,
                                "File exists, overwrite not set");
    return -1;
  }

  // Create parent directories
  char parent[4096];
  strncpy(parent, full_path, sizeof(parent) - 1);
  parent[sizeof(parent) - 1] = '\0';
  char* last_slash = strrchr(parent, '/');
#ifdef _WIN32
  char* last_bslash = strrchr(parent, '\\');
  if (last_bslash && (!last_slash || last_bslash > last_slash))
    last_slash = last_bslash;
#endif
  if (last_slash) {
    *last_slash = '\0';
    isr_mkdir_recursive(parent);
  }

  // Send response: 0x00 for create, 0x01 for overwrite
  uint8_t resp_type = exists ? 0x01 : 0x00;
  if (isr_transmit_send_response(sock, resp_type, NULL) == -1) return -1;

  // Open file for writing
  int fd = isr_open_write(full_path);
  if (fd < 0) {
    // Can't send another response (already sent one), just bail
    return -1;
  }

  // Receive file data
  int64_t result;
  if (header.flags & ISR_FLAG_USE_COMPRESSION) {
    result = isr_receive_file_compressed(sock, fd,
                                          header.uncompressed_block_size,
                                          header.effective_length);
  } else {
    result = isr_receive_file_uncompressed(sock, fd, header.effective_length);
  }
  isr_close(fd);

  // Send result
  if (result == -1) {
    isr_transmit_send_result(sock, ISR_RESULT_OTHER_FAILURE);
    return -1;
  } else if (result == -2) {
    isr_transmit_send_result(sock, ISR_RESULT_CHECKSUM_BAD);
    return -1;
  } else {
    isr_transmit_send_result(sock, ISR_RESULT_OK);
    return 0;
  }
}

int isr_receive_recv_command(net_sock_t sock,
                             isr_recv_command_header_t header,
                             const char* path) {
  // Validate path
  if (isr_validate_path(path) == -1) {
    isr_recv_confirmation_t conf = {0};
    conf.response_type = ISR_RESULT_OTHER_FAILURE;
    conf.effective_length = 0;
    net_send_exact(sock, &conf, sizeof(conf));
    return -1;
  }

  // Resolve full path
  char full_path[4096];
  if (isr_resolve_path(isr_server_root_path, path,
                        full_path, sizeof(full_path)) == -1) {
    isr_recv_confirmation_t conf = {0};
    conf.response_type = ISR_RESULT_OTHER_FAILURE;
    conf.effective_length = 0;
    net_send_exact(sock, &conf, sizeof(conf));
    return -1;
  }

  // Stat the path
  int is_dir = 0;
  uint64_t file_size = 0;
  if (isr_stat(full_path, &is_dir, &file_size) != 0) {
    // Path not found
    isr_recv_confirmation_t conf = {0};
    conf.response_type = 0x03;
    conf.effective_length = 0;
    net_send_exact(sock, &conf, sizeof(conf));
    return 0;
  }

  if (is_dir) {
    // Compute directory listing size
    uint64_t listing_size = 0;
#ifdef _WIN32
    char search_path[4096];
    snprintf(search_path, sizeof(search_path), "%s\\*", full_path);
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(search_path, &ffd);
    if (hFind != INVALID_HANDLE_VALUE) {
      do {
        if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
          continue;
        listing_size += 1 + 8 + 2 + (uint64_t)strlen(ffd.cFileName);
      } while (FindNextFileA(hFind, &ffd));
      FindClose(hFind);
    }
#else
    DIR* dir = opendir(full_path);
    if (dir) {
      struct dirent* de;
      while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
          continue;
        listing_size += 1 + 8 + 2 + (uint64_t)strlen(de->d_name);
      }
      closedir(dir);
    }
#endif

    // Send confirmation header
    isr_recv_confirmation_t conf = {0};
    conf.response_type = 0x01; // directory listing
    conf.effective_length = htobe64(listing_size);
    if (net_send_exact(sock, &conf, sizeof(conf)) == -1) return -1;

    // Wait for client go/no-go
    uint8_t go;
    if (net_recv_exact(sock, &go, 1) == -1) return -1;
    if (go == ISR_RECV_UNABLE) return 0;

    // Stream directory listing
    return isr_transmit_recv_directory_listing(sock, full_path);

  } else {
    // File
    // Send confirmation header
    isr_recv_confirmation_t conf = {0};
    conf.response_type = 0x00; // file data
    conf.effective_length = htobe64(file_size);
    if (net_send_exact(sock, &conf, sizeof(conf)) == -1) return -1;

    // Wait for client go/no-go
    uint8_t go;
    if (net_recv_exact(sock, &go, 1) == -1) return -1;
    if (go == ISR_RECV_UNABLE) return 0;

    // Open and stream file
    int fd = isr_open_read(full_path);
    if (fd < 0) return -1;

    int64_t result;
    if (header.flags & ISR_FLAG_USE_COMPRESSION) {
      result = isr_transmit_file_compressed(sock, fd,
                                             header.uncompressed_block_size);
    } else {
      result = isr_transmit_file_uncompressed(sock, fd);
    }
    isr_close(fd);
    return result >= 0 ? 0 : -1;
  }
}
