// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tiny blocking socket wrapper (POSIX / Winsock) for talking to game-engine
// editors on the local machine or network: UDP multicast discovery and a TCP
// command channel. All calls take timeouts so a missing editor never hangs.
#pragma once

#include <cstdint>
#include <string>

namespace sf::link {

class Socket {
public:
    Socket() = default;
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Socket& operator=(Socket&& o) noexcept;

    // UDP socket joined to `group` on `port`, sending with `ttl` (0 = this machine only).
    bool openMulticast(const std::string& group, uint16_t port, const std::string& bindAddress, int ttl,
                       std::string& error);
    bool sendTo(const std::string& data, const std::string& address, uint16_t port, std::string& error);
    // Receives one datagram; false on timeout or error. `from` gets the sender's IPv4 address.
    bool receive(std::string& data, int timeoutMs, std::string* from = nullptr);

    // TCP listener on `address` (port 0 = any free port); `boundPort` reports the port. `backlog`: connections
    // the system queues before accept.
    bool listen(const std::string& address, uint16_t port, uint16_t& boundPort, std::string& error, int backlog = 1);
    // `from` gets the client's IPv4 address.
    bool accept(Socket& client, int timeoutMs, std::string* from = nullptr);
    // TCP connection to `address`:`port` (blocking; meant for this machine and the local network).
    bool connect(const std::string& address, uint16_t port, std::string& error);
    bool sendAll(const std::string& data, std::string& error);
    // Reads whatever is available (up to `maxBytes`), waiting at most `timeoutMs`.
    bool receiveSome(std::string& data, size_t maxBytes, int timeoutMs);
    // Reads up to `size` bytes into `buffer`: the byte count, 0 on timeout, -1 when the peer closed or on error.
    long long readSome(char* buffer, size_t size, int timeoutMs);

    bool valid() const { return fd_ >= 0; }
    void close();

private:
    bool waitReadable(int timeoutMs) const;
    int64_t fd_ = -1;
};

// Initialises the socket library once (Winsock); harmless elsewhere.
bool initSockets();
// This machine's IPv4 address on the route to `remote` (e.g. its LAN address), or "" if unknown.
std::string localAddressToward(const std::string& remote);
std::string hostName();

} // namespace sf::link
