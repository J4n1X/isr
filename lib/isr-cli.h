#ifndef ISR_CLI_H_
#define ISR_CLI_H_

#include "isr.h"
#include "mininet.h"

#ifdef __cplusplus
extern "C" {
#endif

// Directory entry for parsed directory listings
typedef struct {
  char* name;
  uint64_t size;
  int is_directory;
} isr_cli_dir_entry_t;

// Send a file to a remote server.
// Returns 0 on success, 1 on failure.
int isr_cli_send_file(const char* host, const char* port,
                      const char* remote_path, const char* local_file,
                      uint8_t flags, uint32_t block_size);

// Create a directory on a remote server.
// Returns 0 on success, 1 on failure.
int isr_cli_send_mkdir(const char* host, const char* port,
                       const char* remote_path);

// Recursively send a local directory to a remote server.
// Returns 0 on success, 1 on failure.
int isr_cli_send_directory(const char* host, const char* port,
                           const char* remote_path, const char* local_dir,
                           uint8_t flags, uint32_t block_size);

// Receive from a remote server (non-recursive).
// If the remote path is a directory, prints a listing.
// If it is a file, downloads it to local_dir.
// Returns 0 on success, 1 on failure.
int isr_cli_recv(const char* host, const char* port,
                 const char* remote_path, const char* local_dir,
                 uint8_t flags, uint32_t block_size);

// Download a single file from a remote server.
// Returns 0 on success, -1 on failure.
int isr_cli_download_file(const char* host, const char* port,
                          const char* remote_path, const char* local_path,
                          uint8_t flags, uint32_t block_size);

// Recursively download a directory from a remote server.
// Returns 0 on success, 1 on failure.
int isr_cli_download_directory(const char* host, const char* port,
                               const char* remote_path, const char* local_dir,
                               uint8_t flags, uint32_t block_size);

// Parse a raw directory listing buffer into entries.
// Returns allocated array of entries (caller must free with isr_cli_free_entries).
// Sets *entry_count to the number of entries.
isr_cli_dir_entry_t* isr_cli_parse_directory_listing(const uint8_t* buf,
                                                     size_t buf_len,
                                                     int* entry_count);

// Free entries returned by isr_cli_parse_directory_listing.
void isr_cli_free_entries(isr_cli_dir_entry_t* entries, int count);

#ifdef __cplusplus
}
#endif
#endif /* ISR_CLI_H_ */
