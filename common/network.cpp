#include "network.h"
#include "protocol.h"

#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

using namespace std;

bool recv_all(int fd, void* buf, size_t len) {
    size_t total = 0;
    char* p = static_cast<char*>(buf);
    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue; // interrupted, retry
            return false;
        }
        if (n == 0) return false; // peer closed the connection
        total += static_cast<size_t>(n);
    }
    return true;
}

bool send_all(int fd, const void* buf, size_t len) {
    size_t total = 0;
    const char* p = static_cast<const char*>(buf);
    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

bool send_message(int fd, uint16_t command, uint16_t status, const string& payload) {
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    uint16_t cmd_be = htons(command);
    uint16_t status_be = htons(status);

    if (!send_all(fd, &len_be, sizeof(len_be))) return false;
    if (!send_all(fd, &cmd_be, sizeof(cmd_be))) return false;
    if (!send_all(fd, &status_be, sizeof(status_be))) return false;
    if (!payload.empty() && !send_all(fd, payload.data(), payload.size())) return false;
    return true;
}

bool recv_message(int fd, uint16_t& command, uint16_t& status, string& payload) {
    uint32_t len_be;
    uint16_t cmd_be, status_be;

    if (!recv_all(fd, &len_be, sizeof(len_be))) return false;
    if (!recv_all(fd, &cmd_be, sizeof(cmd_be))) return false;
    if (!recv_all(fd, &status_be, sizeof(status_be))) return false;

    uint32_t len = ntohl(len_be);
    command = ntohs(cmd_be);
    status = ntohs(status_be);

    const uint32_t MAX_PAYLOAD = 16u * 1024u * 1024u; // sanity cap against a garbled length field
    if (len > MAX_PAYLOAD) return false;

    payload.resize(len);
    if (len > 0 && !recv_all(fd, &payload[0], len)) return false;
    return true;
}

int create_listening_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // avoid "address in use" on quick restart

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int accept_connection(int listen_fd) {
    sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (fd < 0) perror("accept");
    return fd;
}

int get_local_port(int fd) {
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        perror("getsockname");
        return -1;
    }
    return ntohs(addr.sin_port);
}

string get_local_ip(int fd) {
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        perror("getsockname");
        return "";
    }
    char buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf))) return "";
    return string(buf);
}

string get_peer_address(int fd) {
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        perror("getpeername");
        return "";
    }
    char buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf))) return "";
    return string(buf) + ":" + to_string(ntohs(addr.sin_port));
}

int connect_to(const string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        hostent* he = gethostbyname(host.c_str()); // not a dotted IP, e.g. "localhost" -- resolve it
        if (!he) {
            fprintf(stderr, "connect_to: could not resolve host '%s'\n", host.c_str());
            close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], static_cast<size_t>(he->h_length));
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

bool parse_host_port(const string& address, string& host, int& port) {
    size_t pos = address.find(':');
    if (pos == string::npos || pos == 0 || pos == address.size() - 1) return false;

    host = address.substr(0, pos);
    string port_str = address.substr(pos + 1);
    for (char c : port_str) {
        if (!isdigit(static_cast<unsigned char>(c))) return false;
    }
    port = atoi(port_str.c_str());
    return port > 0 && port <= 65535;
}
