#ifndef HOSTPORT_H_
#define HOSTPORT_H_

#include <string.h>

// Parse "host:port" into separate host and port strings.
// Returns 0 on success, -1 on failure.
static inline int parse_host_port(const char* input, char* host, size_t host_len,
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

#endif /* HOSTPORT_H_ */
