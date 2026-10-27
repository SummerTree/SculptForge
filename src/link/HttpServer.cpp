// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "link/HttpServer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace sf::link {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool isTokenChar(char c) { // RFC 9110 tchar
    return std::isalnum(static_cast<unsigned char>(c)) || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool parseIpv4(const std::string& text, uint32_t& out) {
    uint32_t value = 0;
    int parts = 0;
    size_t i = 0;
    while (parts < 4) {
        if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) return false;
        uint32_t part = 0;
        size_t digits = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            part = part * 10 + uint32_t(text[i++] - '0');
            if (++digits > 3 || part > 255) return false;
        }
        value = (value << 8) | part;
        if (++parts < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    if (i != text.size()) return false;
    out = value;
    return true;
}

} // namespace

// --- Requests and responses ---

const std::string& HttpRequest::header(const std::string& name) const {
    static const std::string empty;
    for (const auto& [k, v] : headers)
        if (k == name) return v;
    return empty;
}

std::string HttpRequest::param(const std::string& name, const std::string& fallback) const {
    return queryParameter(query, name, fallback);
}

bool HttpRequest::keepAlive() const {
    const std::string connection = lower(header("connection"));
    if (version == "HTTP/1.0") return connection.find("keep-alive") != std::string::npos;
    return connection.find("close") == std::string::npos;
}

HttpResponse HttpResponse::text(int status, std::string body) {
    HttpResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

HttpResponse HttpResponse::json(int status, std::string body) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json";
    r.body = std::move(body);
    return r;
}

HttpResponse HttpResponse::html(std::string body) {
    HttpResponse r;
    r.contentType = "text/html; charset=utf-8";
    r.body = std::move(body);
    return r;
}

HttpResponse HttpResponse::bytes(std::string contentType, std::string body) {
    HttpResponse r;
    r.contentType = std::move(contentType);
    r.body = std::move(body);
    return r;
}

const char* httpReasonPhrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Status";
    }
}

std::string httpResponseHead(const HttpResponse& response, bool keepAlive) {
    std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " + httpReasonPhrase(response.status) + "\r\n";
    head += "Content-Type: " + response.contentType + "\r\n";
    head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    head += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    head += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
    for (const auto& [k, v] : response.headers) head += k + ": " + v + "\r\n";
    return head + "\r\n";
}

// --- Parser ---

HttpRequestParser::State HttpRequestParser::fail(int status, std::string text) {
    state_ = State::Error;
    errorStatus_ = status;
    errorText_ = std::move(text);
    return state_;
}

HttpRequestParser::State HttpRequestParser::feed(const char* data, size_t size) {
    if (state_ == State::Error || state_ == State::Done) {
        if (state_ == State::Done) buffer_.append(data, size); // the next request's bytes
        return state_;
    }
    buffer_.append(data, size);
    return parse();
}

HttpRequestParser::State HttpRequestParser::next() {
    if (state_ != State::Done) return state_;
    request_ = {};
    contentLength_ = 0;
    state_ = State::Head;
    return parse();
}

HttpRequestParser::State HttpRequestParser::parse() {
    if (state_ == State::Head) {
        // Tolerate blank lines before a request (RFC 9112 2.2).
        size_t skip = 0;
        while (skip + 1 < buffer_.size() && buffer_[skip] == '\r' && buffer_[skip + 1] == '\n') skip += 2;
        if (skip) buffer_.erase(0, skip);
        const size_t end = buffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (buffer_.size() > limits_.maxHeadBytes) return fail(431, "Request headers too large");
            return state_;
        }
        if (end + 4 > limits_.maxHeadBytes) return fail(431, "Request headers too large");
        if (!parseHead(buffer_.substr(0, end))) return state_;
        buffer_.erase(0, end + 4);
        state_ = State::Body;
    }
    if (state_ == State::Body) {
        if (buffer_.size() < contentLength_) return state_;
        request_.body.assign(buffer_, 0, contentLength_);
        buffer_.erase(0, contentLength_);
        state_ = State::Done;
    }
    return state_;
}

bool HttpRequestParser::parseHead(const std::string& head) {
    size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    // method SP target SP version
    const size_t s1 = requestLine.find(' ');
    const size_t s2 = s1 == std::string::npos ? std::string::npos : requestLine.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos || requestLine.find(' ', s2 + 1) != std::string::npos) {
        fail(400, "Malformed request line");
        return false;
    }
    request_.method = requestLine.substr(0, s1);
    request_.target = requestLine.substr(s1 + 1, s2 - s1 - 1);
    request_.version = requestLine.substr(s2 + 1);
    if (request_.method.empty() || !std::all_of(request_.method.begin(), request_.method.end(), isTokenChar)) {
        fail(400, "Malformed method");
        return false;
    }
    if (request_.version != "HTTP/1.1" && request_.version != "HTTP/1.0") {
        fail(505, "Unsupported HTTP version");
        return false;
    }
    if (request_.target.empty() || request_.target[0] != '/') {
        fail(400, "Only origin-form targets are accepted");
        return false;
    }
    for (unsigned char c : request_.target)
        if (c <= 0x20 || c >= 0x7f) {
            fail(400, "Invalid character in the target");
            return false;
        }
    const size_t q = request_.target.find('?');
    request_.path = percentDecode(request_.target.substr(0, q));
    request_.query = q == std::string::npos ? std::string() : request_.target.substr(q + 1);
    if (request_.path.find('\0') != std::string::npos) {
        fail(400, "Invalid path");
        return false;
    }

    bool sawLength = false;
    while (lineEnd != std::string::npos) {
        const size_t start = lineEnd + 2;
        lineEnd = head.find("\r\n", start);
        const std::string line = head.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start);
        if (line.empty()) continue;
        if (line[0] == ' ' || line[0] == '\t') {
            fail(400, "Folded headers are not accepted");
            return false;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            fail(400, "Malformed header");
            return false;
        }
        std::string name = line.substr(0, colon);
        if (!std::all_of(name.begin(), name.end(), isTokenChar)) {
            fail(400, "Malformed header name");
            return false;
        }
        std::string value = trim(line.substr(colon + 1));
        for (unsigned char c : value)
            if ((c < 0x20 && c != '\t') || c == 0x7f) {
                fail(400, "Invalid character in a header");
                return false;
            }
        if (request_.headers.size() >= limits_.maxHeaders) {
            fail(431, "Too many headers");
            return false;
        }
        name = lower(name);
        if (name == "transfer-encoding") {
            fail(501, "Chunked bodies are not accepted; send a Content-Length");
            return false;
        }
        if (name == "content-length") {
            if (value.empty() || value.size() > 15 || !std::all_of(value.begin(), value.end(), [](char c) {
                    return std::isdigit(static_cast<unsigned char>(c));
                })) {
                fail(400, "Invalid Content-Length");
                return false;
            }
            const size_t length = size_t(std::stoull(value));
            if (sawLength && length != contentLength_) {
                fail(400, "Conflicting Content-Length headers");
                return false;
            }
            sawLength = true;
            contentLength_ = length;
            if (contentLength_ > limits_.maxBodyBytes) {
                fail(413, "Request body too large");
                return false;
            }
        }
        request_.headers.emplace_back(std::move(name), std::move(value));
    }
    return true;
}

// --- Helpers ---

std::string percentDecode(const std::string& text, bool plusIsSpace) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%' && i + 2 < text.size()) {
            const int hi = hexValue(text[i + 1]), lo = hexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += char(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += plusIsSpace && c == '+' ? ' ' : c;
    }
    return out;
}

std::string percentEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string queryParameter(const std::string& query, const std::string& name, const std::string& fallback) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(pos, amp - pos);
        const size_t eq = pair.find('=');
        if (percentDecode(pair.substr(0, eq), true) == name)
            return eq == std::string::npos ? std::string() : percentDecode(pair.substr(eq + 1), true);
        pos = amp + 1;
    }
    return fallback;
}

bool isLocalNetworkAddress(const std::string& ipv4) {
    uint32_t a = 0;
    if (!parseIpv4(ipv4, a)) return false;
    const auto in = [a](uint32_t net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };
    return in(0x7F000000u, 8)     // 127/8 loopback
           || in(0x0A000000u, 8)  // 10/8
           || in(0xAC100000u, 12) // 172.16/12
           || in(0xC0A80000u, 16) // 192.168/16
           || in(0xA9FE0000u, 16) // 169.254/16 link-local
           || in(0x64400000u, 10); // 100.64/10 shared (carrier NAT, Tailscale)
}

std::vector<std::string> localNetworkAddresses() {
    // The address the system would use toward a few typical destinations: the default route first, then the
    // usual home and office ranges. Connecting a UDP socket sends nothing.
    std::vector<std::string> out;
    for (const char* probe : {"8.8.8.8", "192.168.1.1", "192.168.0.1", "10.0.0.1", "172.16.0.1", "100.100.100.100"}) {
        const std::string a = localAddressToward(probe);
        if (a.empty() || a.rfind("127.", 0) == 0 || !isLocalNetworkAddress(a)) continue;
        if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
    }
    return out;
}

HttpClientResponse httpLocalRequest(const std::string& address, uint16_t port, const std::string& method,
                                    const std::string& target, const std::string& body,
                                    const std::vector<std::pair<std::string, std::string>>& headers, int timeoutMs) {
    HttpClientResponse out;
    Socket s;
    if (!s.connect(address, port, out.error)) return out;
    std::string req = method + " " + target + " HTTP/1.1\r\nHost: " + address + ":" + std::to_string(port) +
                      "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
    req += "\r\n" + body;
    if (!s.sendAll(req, out.error)) return out;
    std::string data;
    std::vector<char> buffer(64 * 1024);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    size_t headEnd = std::string::npos, length = std::string::npos;
    while (std::chrono::steady_clock::now() < deadline) {
        const long long n = s.readSome(buffer.data(), buffer.size(), 100);
        if (n < 0) break;
        if (n > 0) data.append(buffer.data(), size_t(n));
        if (headEnd == std::string::npos && (headEnd = data.find("\r\n\r\n")) != std::string::npos) {
            const std::string head = data.substr(0, headEnd);
            if (head.size() > 12) out.status = std::atoi(head.c_str() + 9);
            size_t pos = head.find("\r\n");
            while (pos != std::string::npos) {
                const size_t start = pos + 2, end = head.find("\r\n", start);
                const std::string line = head.substr(start, end == std::string::npos ? std::string::npos : end - start);
                const size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    out.headers.emplace_back(lower(line.substr(0, colon)), trim(line.substr(colon + 1)));
                    if (out.headers.back().first == "content-length") length = size_t(std::atoll(out.headers.back().second.c_str()));
                }
                pos = end;
            }
        }
        if (headEnd != std::string::npos && length != std::string::npos && data.size() >= headEnd + 4 + length) break;
    }
    if (headEnd == std::string::npos) {
        out.status = 0;
        out.error = "No answer";
        return out;
    }
    out.body = data.substr(headEnd + 4, length == std::string::npos ? std::string::npos : length);
    return out;
}

// --- Server ---

bool HttpServer::start(const Options& options, Handler handler, std::string& error) {
    stop();
    options_ = options;
    handler_ = std::move(handler);
    uint16_t bound = 0;
    bool ok = false;
    std::string err;
    if (options.port != 0) {
        for (int i = 0; i < std::max(1, options.portTries) && !ok; ++i)
            ok = listener_.listen(options.bindAddress, uint16_t(options.port + i), bound, err, 16);
    }
    if (!ok) ok = listener_.listen(options.bindAddress, 0, bound, err, 16);
    if (!ok) {
        error = err;
        return false;
    }
    port_ = bound;
    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::stop() {
    if (!running_ && !acceptThread_.joinable()) return;
    running_ = false;
    if (acceptThread_.joinable()) acceptThread_.join();
    reapFinished(true);
    listener_.close();
    port_ = 0;
}

void HttpServer::reapFinished(bool all) {
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (all || *it->done) {
            if (it->thread.joinable()) it->thread.join();
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void HttpServer::acceptLoop() {
    while (running_) {
        reapFinished(false);
        auto client = std::make_shared<Socket>();
        std::string peer;
        if (!listener_.accept(*client, 200, &peer)) continue;
        std::string err;
        if (options_.localNetworkOnly && !isLocalNetworkAddress(peer)) {
            HttpResponse r = HttpResponse::text(403, "SculptForge only answers devices on the local network.\n");
            client->sendAll(httpResponseHead(r, false) + r.body, err);
            continue;
        }
        if (active_ >= options_.maxConnections) {
            HttpResponse r = HttpResponse::text(503, "Too many connections, try again.\n");
            r.headers.emplace_back("Retry-After", "1");
            client->sendAll(httpResponseHead(r, false) + r.body, err);
            continue;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        ++active_;
        connections_.push_back({std::thread([this, client, peer, done] {
                                    serve(*client, peer);
                                    client->close();
                                    --active_;
                                    *done = true;
                                }),
                                done});
    }
}

void HttpServer::serve(Socket& client, const std::string& peer) {
    HttpRequestParser parser(options_.limits);
    std::string err;
    std::vector<char> buffer(256 * 1024);
    auto idleSince = std::chrono::steady_clock::now();
    while (running_) {
        if (!parser.done() && !parser.failed()) {
            const long long n = client.readSome(buffer.data(), buffer.size(), 250);
            if (n < 0) return; // closed
            if (n == 0) {
                if (std::chrono::steady_clock::now() - idleSince > std::chrono::milliseconds(options_.idleTimeoutMs)) return;
                continue;
            }
            idleSince = std::chrono::steady_clock::now();
            parser.feed(buffer.data(), size_t(n));
        }
        if (parser.failed()) {
            HttpResponse r = HttpResponse::text(parser.errorStatus(), parser.errorText() + "\n");
            client.sendAll(httpResponseHead(r, false) + r.body, err);
            return;
        }
        if (!parser.done()) continue;
        HttpRequest& request = parser.request();
        request.peer = peer;
        HttpResponse response;
        try {
            response = handler_ ? handler_(request) : HttpResponse::text(404, "Not found\n");
        } catch (const std::exception& e) {
            response = HttpResponse::text(500, std::string("Internal error: ") + e.what() + "\n");
        }
        const bool keep = request.keepAlive() && running_;
        std::string head = httpResponseHead(response, keep);
        if (request.method == "HEAD") response.body.clear();
        // Small responses go out in one send; big ones (models) after their head.
        if (response.body.size() < 64 * 1024) {
            if (!client.sendAll(head + response.body, err)) return;
        } else if (!client.sendAll(head, err) || !client.sendAll(response.body, err)) {
            return;
        }
        if (!keep) return;
        idleSince = std::chrono::steady_clock::now();
        parser.next();
    }
}

} // namespace sf::link
