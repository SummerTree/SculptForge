// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small HTTP/1.1 server for the phone link (link/PhoneLink.h): the phone's browser loads the capture page and
// talks to the desktop over plain HTTP on the local network. Requests are parsed incrementally with hard limits
// (head size, body size, header count), bodies need a Content-Length (no chunked uploads), connections are kept
// alive, and only clients with local-network addresses are served unless told otherwise. One thread accepts,
// one thread serves each connection (at most `maxConnections`).
#pragma once

#include "link/Socket.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sf::link {

struct HttpRequest {
    std::string method;  // "GET", "POST", "PUT", ...
    std::string target;  // as sent: "/api/file/3?offset=0"
    std::string path;    // percent-decoded path without the query: "/api/file/3"
    std::string query;   // raw query without '?'
    std::string version; // "HTTP/1.1"
    std::vector<std::pair<std::string, std::string>> headers; // names lower-cased, in order
    std::string body;
    std::string peer; // client IPv4 address

    // The first header called `name` (lower case), or "".
    const std::string& header(const std::string& name) const;
    // A query parameter, percent-decoded ('+' is a space); `fallback` when absent.
    std::string param(const std::string& name, const std::string& fallback = {}) const;
    bool keepAlive() const;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers; // extra headers

    static HttpResponse text(int status, std::string body);
    static HttpResponse json(int status, std::string body);
    static HttpResponse html(std::string body);
    static HttpResponse bytes(std::string contentType, std::string body);
};

// The status line and headers (Content-Length, Connection, no-store caching, nosniff) for a response.
std::string httpResponseHead(const HttpResponse& response, bool keepAlive);
const char* httpReasonPhrase(int status);

struct HttpLimits {
    size_t maxHeadBytes = 16 * 1024;      // request line + headers
    size_t maxHeaders = 64;
    size_t maxBodyBytes = 17 * 1024 * 1024; // the largest upload chunk plus room
};

// Incremental request parser: feed bytes as they arrive. After `done()`, `request()` holds the request and any
// bytes past it stay buffered for the next one (pipelining); call `next()` to start parsing it.
class HttpRequestParser {
public:
    enum class State { Head, Body, Done, Error };

    explicit HttpRequestParser(HttpLimits limits = {}) : limits_(limits) {}
    // Appends `size` bytes and parses as far as possible. Returns the state.
    State feed(const char* data, size_t size);
    State state() const { return state_; }
    bool done() const { return state_ == State::Done; }
    bool failed() const { return state_ == State::Error; }
    // On Error: the status to answer with (400, 413, 431, 501, 505) and why.
    int errorStatus() const { return errorStatus_; }
    const std::string& errorText() const { return errorText_; }
    // The declared body length (valid once the head is parsed), for early rejection of oversized uploads.
    size_t contentLength() const { return contentLength_; }
    HttpRequest& request() { return request_; }
    // Starts the next request with the bytes left over from the last one.
    State next();

private:
    State fail(int status, std::string text);
    State parse();
    bool parseHead(const std::string& head);

    HttpLimits limits_;
    State state_ = State::Head;
    std::string buffer_;
    HttpRequest request_;
    size_t contentLength_ = 0;
    int errorStatus_ = 0;
    std::string errorText_;
};

// Percent-decoding ('+' as space when `plusIsSpace`); invalid escapes are kept as they are.
std::string percentDecode(const std::string& text, bool plusIsSpace = false);
std::string percentEncode(const std::string& text);
// The `name` parameter of a query string ("a=1&b=2"), decoded.
std::string queryParameter(const std::string& query, const std::string& name, const std::string& fallback = {});

// Private (RFC 1918), loopback, link-local and shared (100.64/10: carrier NAT, Tailscale) IPv4 addresses.
bool isLocalNetworkAddress(const std::string& ipv4);
// This computer's IPv4 addresses on its local networks (the one with the default route first).
std::vector<std::string> localNetworkAddresses();

// A minimal client for this server (tests, the UI tests' scripted phone): one request on a new connection,
// waiting at most `timeoutMs` for the whole answer. `status` 0 = no answer.
struct HttpClientResponse {
    int status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers; // names lower-cased
    std::string error;
};
HttpClientResponse httpLocalRequest(const std::string& address, uint16_t port, const std::string& method,
                                    const std::string& target, const std::string& body = {},
                                    const std::vector<std::pair<std::string, std::string>>& headers = {},
                                    int timeoutMs = 10000);

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    struct Options {
        uint16_t port = 0;          // 0 = any free port
        int portTries = 10;         // try port, port + 1, ... before taking any free one
        std::string bindAddress = "0.0.0.0";
        bool localNetworkOnly = true; // refuse clients outside the local network (403)
        int maxConnections = 16;
        int idleTimeoutMs = 15000;  // a kept-alive connection with no request closes after this
        HttpLimits limits;
    };

    HttpServer() = default;
    ~HttpServer() { stop(); }
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Starts listening and serving on background threads. `handler` runs on connection threads (concurrently).
    bool start(const Options& options, Handler handler, std::string& error);
    // Closes the listener and every connection, and waits for their threads.
    void stop();
    bool running() const { return running_; }
    uint16_t port() const { return port_; }
    int activeConnections() const { return active_; }

private:
    void acceptLoop();
    void serve(Socket& client, const std::string& peer);
    void reapFinished(bool all);

    struct Connection {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    Options options_;
    Handler handler_;
    Socket listener_;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> active_{0};
    std::thread acceptThread_;
    std::vector<Connection> connections_; // accept thread only (and stop, after it has ended)
};

} // namespace sf::link
