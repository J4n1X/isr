# I Send & Receive - Simple File Transfer Tool

## Broad idea

Sometimes, you gotta send or receive files, and you wanna do that in a stupidly simple way. No uploading to some drive. No annoying SCP or SFTP or FTP, whatever. Just send and receive. Well, this aims to solve that problem.

The receiver will be open at some place in the file system, serving a specified directory. The client can then tailor a specific way it wants to receive stuff (with sane defaults of course so you don't have to be pedantic all the time) and it can do it as I will specify now.

## The protocol

The client has only two things it can do: Send and Receive. The client can send files, and can receive directory listings or file data, though the method for receiving either is the exact same. Compression is achieved via LZ4. Any communication with the server must always begin with a request type byte, which will specify whether to send (0x01) or to receive (0x00)

### Sending

The client specifies a path where a file should be put, if compression should be used and the size of the uncompressed blocks (if compressed). Parent directories are created automatically if needed.

The client will send a command that has the following header, which is of a variable size:

```c
struct send_command_header {
  unsigned char flags; // Bit 0: Use compression. Bit 1: Overwrite file if it exists. Bit 2: Create a directory here. The rest are reserved.
  unsigned short path_length; // Maximum path length on NTFS would be a signed short, EXT4 has 4096. This is enough.
  char path[path_length];
  unsigned int uncompressed_block_size; // How big a block may be while uncompressed. 
  unsigned long effective_length; // How big the actual file is
};
```

The server will return a response that looks as such:

```c
struct send_response_header {
  unsigned char response_type; // Can be 0x00 if the file will be created, or 0x01 if the file will be overwritten. In case of a failure, the response code is 0xFF and 254 bytes are available to be read for an error response. Otherwise, this space will be empty.
  char response_data[254]; // Further information on errors can be put in here.
};
```

After evaluating the response, the client will stream the data.

When it is done, it expects another packet from the server of the same type as `send_response_header`, but the return codes will be as such:
 - 0x00: Success, no further information
 - 0x01: Checksum bad, no further information
 - 0x02: Server cancelled the transmission, no further information (May happen if the server is killed during an active transmission)
 - 0xFF: System Error, check response_data payload

### Receiving

The client specifies which file or directory to get, defining if compression should be used, and how big the uncompressed blocks of data should be (if compressed). 

The format for this will be as such:

```c
struct recv_command_header {
  unsigned char flags; // Bit 0: Use compression. Bit 1: Overwrite file if it exists. The rest are reserved.
  unsigned short path_length; // Maximum path length on NTFS would be a signed short, EXT4 has 4096. This is enough.
  char path[path_length];
  unsigned int uncompressed_block_size; // How big a block may be while uncompressed. 
};
```


The server will return a 25-byte header, which is as such:

```c
struct recv_response_header {
  unsigned char response_type; // Can be 0x00 for file data, 0x01 for a directory listing, 0x03 if the path does not exist, 0xFF for a different failure, in which case, compression is ignored and the checksum is zero. Directory listings are always uncompressed.
  unsigned long effective_length; // How large the file is if uncompressed.
};
```

The client must evaluate if it can receive the data, or if a failure happened. If it is ready to receive the data, it will confirm by sending a single byte which contains a boolean:

 - True: OK, begin data stream
 - False: Not able to handle this response, cancel request.

It will then begin with the data stream. The client will not confirm if the operation was successful to the server.

### The data stream

The response format changes based on if compression is used. If compression is used, then one more QWORD is used to inform about the decompressed, effective size of the file. 
```c
unsigned long length; // Length of the compressed byte stream.
```

Following this prelude, every block of data will look like this:

```c
struct compressed_block {
  unsigned int block_length; // We will restrict the maximum size of a compressed block to 4 gigabytes. 
  char data[block_length];
};
```

If no compression is used, instead, the data is simply streamed all in one go. 

At the end of the stream, 8 more bytes will be given out as a checksum.