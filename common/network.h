#ifndef NETWORK_H
#define NETWORK_H

#include <cstdint>
#include <string>


bool recv_all(int fd, void* buf, size_t len);
bool send_all(int fd, const void* buf, size_t len);

bool send_message(int fd, uint16_t command, uint16_t status, const std::string& payload);
bool recv_message(int fd, uint16_t& command, uint16_t& status, std::string& payload);

int create_listening_socket(int port); // binds on all local interfaces, returns -1 on failure
int accept_connection(int listen_fd);  // wraps accept() so callers don't need <sys/socket.h>
int connect_to(const std::string& host, int port); // host may be a dotted IP or hostname


int get_local_port(int fd);
std::string get_local_ip(int fd);

std::string get_peer_address(int fd);

// Splits "IP:PORT" into host and port; false if malformed.
bool parse_host_port(const std::string& address, std::string& host, int& port);

#endif // NETWORK_H
