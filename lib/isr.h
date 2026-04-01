#ifndef ISR_H_
#define ISR_H_

#include <stdint.h>
#include <string.h>
#include "packed_struct.h"
#include "mininet.h"

#ifdef __cplusplus
extern "C" {
#endif

// Command header sent by the client to initiate a file transfer.
// It is followed by the path string (not null-terminated)
PACK_STRUCT_BEGIN
typedef struct {
  // Bit 0: Use compression. Bit 1: Overwrite file if it exists.
  // Bit 2: Create a directory here. The rest are reserved.
  uint8_t flags; 
  // Maximum path length on NTFS would be a signed short, EXT4 has 4096. 
  uint16_t path_length;
  // How big a block may be while uncompressed. 
  uint32_t uncompressed_block_size;
  // How big the actual file is.
  uint64_t effective_length;

} PACK_STRUCT_ATTR isr_send_command_header_t;
PACK_STRUCT_END

PACK_STRUCT_BEGIN
typedef struct {
  // Can be 0x00 if the file will be created, or 0x01 if the file will be 
  // overwritten. In case of a failure, the response code is 0xFF 
  // and 254 bytes are available to be read for an error response. 
  // Otherwise, this space will be empty.
  uint8_t response_type;
  char response_data[254]; // Further information on errors can be put in here.
} PACK_STRUCT_ATTR isr_response_t;
PACK_STRUCT_END

PACK_STRUCT_BEGIN
typedef struct {
  // Bit 0: Use compression. Bit 1: Overwrite file if it exists. 
  // The rest are reserved.
  uint8_t flags;
  uint16_t path_length;
  uint32_t uncompressed_block_size;
} PACK_STRUCT_ATTR isr_recv_command_header_t;
PACK_STRUCT_END

PACK_STRUCT_BEGIN
typedef struct {
  // Can be 0x00 for file data, 0x01 for a directory listing,
  // 0x03 if the path does not exist, 0xFF for a different failure, 
  // in which case, compression is ignored and the checksum is zero. 
  // Directory listings are always uncompressed.
  uint8_t response_type;
  // How large the file will be will be when uncompressed.
  uint64_t effective_length;
} PACK_STRUCT_ATTR isr_recv_confirmation_t;
PACK_STRUCT_END

// Client transmit confirmation types
#define ISR_RECV_GO (uint8_t)0x01
#define ISR_RECV_UNABLE (uint8_t)0x00

// Transmit result types (for both send and recv)
#define ISR_RESULT_OK (uint8_t)0x00
#define ISR_RESULT_CHECKSUM_BAD (uint8_t)0x01
// When this is sent, there is also going to be info in the
// response_data field of the recv response struct.
#define ISR_RESULT_OTHER_FAILURE (uint8_t)0xFF

// Flags
#define ISR_FLAG_USE_COMPRESSION (uint8_t)0x01
#define ISR_FLAG_OVERWRITE (uint8_t)0x02
#define ISR_FLAG_CREATE_DIRECTORY (uint8_t)0x04

// Command types
#define ISR_COMMAND_SEND (uint8_t)0x01
#define ISR_COMMAND_RECV (uint8_t)0x00


// Directory listing helper struct
typedef struct {
  char *name;
  uint64_t size;
  uint8_t is_directory: 1;
  // Unused for now.
  uint8_t is_symlink: 1;
  uint8_t reserved: 6;
} isr_direntry_t;

// Used for the end checksum calculation
static inline uint64_t _isr_count_byte_values(uint8_t* data, uint32_t length){
  uint64_t count = 0;
  for(uint32_t i = 0; i < length; i++){
    count += data[i];
  }
  return count;
}

static inline isr_response_t isr_response_from_error(const char* error_message) {
  isr_response_t response;
  response.response_type = ISR_RESULT_OTHER_FAILURE;
  strncpy(response.response_data, error_message, sizeof(response.response_data) - 1);
  response.response_data[sizeof(response.response_data) - 1] = '\0'; // Ensure null-termination
  return response;
}

static inline isr_response_t isr_response_from_result(uint8_t result_type) {
  isr_response_t response = {0}; // Initialize all fields to zero
  response.response_type = result_type;
  return response;
}

//
// Transmission functions. 
// 
// All of these return 0 on success, -1 on failure, 
// except for the file transmission functions, which return the checksum on success and -1 on failure.
//

// Stream a file over the network, uncompressed. 
int64_t isr_transmit_file_uncompressed(net_sock_t sock, int fd);

// Stream a file over the network, compressed with LZ4.
int64_t isr_transmit_file_compressed(net_sock_t sock, int fd, 
                                   uint32_t uncompressed_block_size);

// Transmit the send command to the server. 
int isr_transmit_send_command(net_sock_t sock, const char* path, 
                              uint8_t flags, 
                              uint32_t uncompressed_block_size, 
                              uint64_t effective_length);

// Transmit the send response to the client.
int isr_transmit_send_response(net_sock_t sock, uint8_t response_type, 
                               const char* response_data);

// Transmit the send result to the client.
// Used on both the client and the server.
int isr_transmit_send_result(net_sock_t sock, uint8_t response_type);

// Transmit the receive command to the server.
// Returns 0 on success, -1 on failure.
int isr_transmit_recv_command(net_sock_t sock, const char* path, 
                              uint8_t flags, 
                              uint32_t uncompressed_block_size);

int isr_transmit_recv_directory_listing(net_sock_t sock, const char* path);

//
// Reception functions.
//
// All of these return 0 on success, -1 on failure,
// except for the file reception functions,
// which return the checksum on success and -1 on failure.
//

// Receive a file from the network, uncompressed.
// Returns checksum on success, -1 on I/O error, -2 on checksum mismatch.
int64_t isr_receive_file_uncompressed(net_sock_t sock, int fd,
                                       uint64_t effective_length);

// Receive a file from the network, compressed with LZ4.
// Returns checksum on success, -1 on I/O error, -2 on checksum mismatch.
int64_t isr_receive_file_compressed(net_sock_t sock, int fd,
                                     uint32_t uncompressed_block_size,
                                     uint64_t effective_length);

// Receive and display a directory listing from the network.
int isr_receive_directory_listing(net_sock_t sock, uint64_t effective_length);

// Delegates to the appropriate receive function based on the command type.
int isr_receive_command(net_sock_t sock);

int isr_receive_send_command(net_sock_t sock, isr_send_command_header_t header,
                             const char* path);

int isr_receive_recv_command(net_sock_t sock, isr_recv_command_header_t header,
                             const char* path);

// Set the server root directory. Must be called before isr_receive_command().
void isr_set_server_root(const char* root);



#ifdef __cplusplus
}
#endif
#endif /* ISR_H_ */