#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using Clock = std::chrono::steady_clock;

// ---------------- Shutdown state ----------------

std::atomic<bool> g_stopping{false};
std::atomic<int> g_activeThreads{0};

BOOL WINAPI consoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_BREAK_EVENT)
    {
        g_stopping = true;
        return TRUE; // we handle it ourselves; main() drains and exits
    }
    return FALSE;
}

// ---------------- Settings ----------------

bool g_allowPrivate = false;
int g_idleTimeoutMs = 120000;

constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr int kFirstRequestTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSocketIoTimeoutMs = 60000; // a single send()/recv() must not hang longer than this
constexpr int kMaxConnections = 512;
constexpr int kMaxPerClientIp = 128;

struct Cidr
{
    uint32_t base;
    uint32_t mask;
};
std::vector<Cidr> g_allowedClients;

// ---------------- Logging ----------------
//
// Every line looks like:
//   [18:42:03] TAG     detail...
// TAG is fixed-width so columns line up in the console.

std::mutex logMutex;

std::string timestamp()
{
    time_t t = time(nullptr);
    tm local{};
    localtime_s(&local, &t);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

// Right-pads a tag to a fixed width, e.g. "TUNNEL" -> "TUNNEL  "
std::string tag(const std::string &t)
{
    std::string out = t;
    while (out.size() < 8)
        out += ' ';
    return out;
}

void logRaw(const std::string &line)
{
    std::lock_guard<std::mutex> lock(logMutex);
    std::cout << "[" << timestamp() << "] " << line << "\n";
}

void logEvent(const std::string &tagName, const std::string &detail)
{
    logRaw(tag(tagName) + detail);
}

// Human-readable byte counts: 850 B, 2.3 KB, 47.1 MB
std::string humanBytes(long long n)
{
    char buf[32];
    if (n < 1024)
    {
        snprintf(buf, sizeof(buf), "%lld B", n);
    }
    else if (n < 1024LL * 1024)
    {
        snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024.0));
    }
    return buf;
}

std::string cleanForLog(const std::string &s)
{
    std::string out = s.substr(0, 100);
    for (char &c : out)
        if (c < 0x20 || c > 0x7E)
            c = '?';
    return out;
}

// ---------------- Small helpers ----------------

std::string envOr(const char *name, const char *fallback)
{
    const char *v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

bool allDigits(const std::string &s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    return true;
}

bool parsePort(const std::string &s, int &out)
{
    if (!allDigits(s) || s.size() > 5)
        return false;
    out = std::stoi(s);
    return out >= 1 && out <= 65535;
}

bool isAllowedPort(bool isTunnel, int port)
{
    return isTunnel ? (port == 443) : (port == 80);
}

void sendStatus(SOCKET s, const char *status)
{
    std::string r = std::string("HTTP/1.1 ") + status +
                    "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(s, r.c_str(), (int)r.size(), 0);
}

timeval toTimeval(int ms)
{
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return tv;
}

int msUntil(Clock::time_point deadline)
{
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return ms < 0 ? 0 : (int)ms;
}

// Case-insensitive "does line start with prefix"
bool startsWithCI(const std::string &line, const std::string &prefix)
{
    if (line.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); i++)
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)prefix[i]))
            return false;
    return true;
}

// ---------------- Accounting hook ----------------
//
// Every byte that crosses the proxy passes through here exactly once.
// Today it does nothing. This is where the Redis slice check and the
// 64 kbps throttle will plug in later, without changing the relay loop.

enum class Direction
{
    Up,
    Down
};

void onBytes(Direction /*dir*/, long long /*n*/)
{
    // placeholder for metering
}

// ---------------- Socket that closes itself (RAII) ----------------

class Socket
{
public:
    Socket() = default;
    explicit Socket(SOCKET s) : s_(s) {}
    ~Socket() { reset(); }

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    Socket(Socket &&o) noexcept : s_(o.s_) { o.s_ = INVALID_SOCKET; }
    Socket &operator=(Socket &&o) noexcept
    {
        if (this != &o)
        {
            reset();
            s_ = o.s_;
            o.s_ = INVALID_SOCKET;
        }
        return *this;
    }

    SOCKET get() const { return s_; }
    bool valid() const { return s_ != INVALID_SOCKET; }
    void reset()
    {
        if (s_ != INVALID_SOCKET)
        {
            closesocket(s_);
            s_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET s_ = INVALID_SOCKET;
};

void applyIoTimeouts(SOCKET s)
{
    DWORD t = kSocketIoTimeoutMs; // Windows wants milliseconds as a DWORD here
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
}

// ---------------- Connection limits ----------------

std::mutex g_connMutex;
int g_totalConns = 0;
std::map<uint32_t, int> g_perIpConns;

bool acquireSlot(uint32_t ip)
{
    std::lock_guard<std::mutex> lock(g_connMutex);
    if (g_totalConns >= kMaxConnections)
        return false;
    auto it = g_perIpConns.find(ip);
    int current = (it == g_perIpConns.end()) ? 0 : it->second;
    if (current >= kMaxPerClientIp)
        return false;
    g_perIpConns[ip] = current + 1;
    g_totalConns++;
    return true;
}

void releaseSlot(uint32_t ip)
{
    std::lock_guard<std::mutex> lock(g_connMutex);
    g_totalConns--;
    auto it = g_perIpConns.find(ip);
    if (it != g_perIpConns.end() && --(it->second) <= 0)
        g_perIpConns.erase(it);
}

struct SlotGuard
{
    uint32_t ip;
    explicit SlotGuard(uint32_t i) : ip(i) {}
    ~SlotGuard() { releaseSlot(ip); }
};

// ---------------- Destination filtering ----------------

bool isBlockedIPv4(uint32_t ip)
{
    struct Range
    {
        uint32_t base;
        int bits;
    };
    static const Range blocked[] = {
        {0x00000000, 8}, {0x0A000000, 8}, {0x64400000, 10}, {0x7F000000, 8}, {0xA9FE0000, 16}, {0xAC100000, 12}, {0xC0000000, 24}, {0xC0A80000, 16}, {0xC6120000, 15}, {0xE0000000, 4}, {0xF0000000, 4}};
    for (const Range &r : blocked)
    {
        uint32_t mask = (r.bits == 0) ? 0u : (0xFFFFFFFFu << (32 - r.bits));
        if ((ip & mask) == (r.base & mask))
            return true;
    }
    return false;
}

bool isBlockedAddress(const sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return isBlockedIPv4(ntohl(((const sockaddr_in *)sa)->sin_addr.s_addr));

    if (sa->sa_family == AF_INET6)
    {
        const unsigned char *b = ((const sockaddr_in6 *)sa)->sin6_addr.s6_addr;

        bool first10Zero = true;
        for (int i = 0; i < 10; i++)
            if (b[i] != 0)
                first10Zero = false;
        if (first10Zero && b[10] == 0xFF && b[11] == 0xFF)
        {
            uint32_t v4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                          ((uint32_t)b[14] << 8) | (uint32_t)b[15];
            return isBlockedIPv4(v4);
        }

        bool first15Zero = true;
        for (int i = 0; i < 15; i++)
            if (b[i] != 0)
                first15Zero = false;
        if (first15Zero && b[15] <= 1)
            return true; // :: and ::1
        if ((b[0] & 0xFE) == 0xFC)
            return true; // fc00::/7
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80)
            return true; // fe80::/10
        if (b[0] == 0xFF)
            return true; // multicast
        return false;
    }
    return true;
}

// ---------------- Source access control ----------------

std::vector<Cidr> parseCidrList(const std::string &text)
{
    std::vector<Cidr> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        size_t slash = item.find('/');
        std::string ipText = item.substr(0, slash);
        int bits = 32;
        bool ok = true;
        if (slash != std::string::npos)
        {
            std::string b = item.substr(slash + 1);
            if (!allDigits(b) || b.size() > 2)
                ok = false;
            else
                bits = std::stoi(b);
        }
        in_addr a{};
        if (!ok || bits > 32 || inet_pton(AF_INET, ipText.c_str(), &a) != 1)
        {
            logEvent("CONFIG", "ignoring bad CIDR entry: " + cleanForLog(item));
            continue;
        }
        uint32_t mask = (bits == 0) ? 0u : (0xFFFFFFFFu << (32 - bits));
        out.push_back(Cidr{ntohl(a.s_addr) & mask, mask});
    }
    return out;
}

bool isClientAllowed(uint32_t ip)
{
    for (const Cidr &c : g_allowedClients)
        if ((ip & c.mask) == c.base)
            return true;
    return false;
}

// ---------------- Reading the request ----------------

enum class ReadResult
{
    Ok,
    Closed,
    Timeout,
    TooLarge,
    Error
};

struct Head
{
    std::string headers;  // up to and including the blank line
    std::string leftover; // bytes that arrived right behind the headers
};

int waitReadable(SOCKET s, int timeoutMs)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval tv = toTimeval(timeoutMs);
    return select(0, &fds, nullptr, nullptr, &tv);
}

ReadResult readHeaders(SOCKET s, Head &out, int timeoutMs)
{
    Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string data;
    char buf[4096];

    while (true)
    {
        int remaining = msUntil(deadline);
        if (remaining <= 0)
            return ReadResult::Timeout;

        int w = waitReadable(s, remaining);
        if (w == 0)
            return ReadResult::Timeout;
        if (w < 0)
            return ReadResult::Error;

        int n = recv(s, buf, sizeof(buf), 0);
        if (n == 0)
            return ReadResult::Closed;
        if (n < 0)
            return ReadResult::Error;

        data.append(buf, n);

        size_t end = data.find("\r\n\r\n");
        if (end != std::string::npos)
        {
            if (end + 4 > kMaxHeaderBytes)
                return ReadResult::TooLarge;
            out.headers = data.substr(0, end + 4);
            out.leftover = data.substr(end + 4);
            return ReadResult::Ok;
        }
        if (data.size() > kMaxHeaderBytes)
            return ReadResult::TooLarge;
    }
}

// Splits headers into lines (each still ending in \r\n, except we trim it),
// dropping any Proxy-* header and the request line itself.
std::vector<std::string> headerLinesWithoutProxy(const std::string &headers)
{
    std::vector<std::string> lines;
    size_t pos = headers.find("\r\n"); // skip the request line
    pos += 2;
    while (pos < headers.size())
    {
        size_t next = headers.find("\r\n", pos);
        if (next == std::string::npos)
            break;
        std::string line = headers.substr(pos, next - pos);
        pos = next + 2;
        if (line.empty())
            break; // blank line = end of headers
        if (!startsWithCI(line, "Proxy-"))
            lines.push_back(line);
    }
    return lines;
}

// ---------------- sendAll with a real reason ----------------

enum class SendResult
{
    Ok,
    Closed,  // peer closed / reset
    Timeout, // SO_SNDTIMEO fired
    Error
};

SendResult sendAllEx(SOCKET s, const char *data, int n)
{
    int sent = 0;
    while (sent < n)
    {
        int r = send(s, data + sent, n - sent, 0);
        if (r > 0)
        {
            sent += r;
            continue;
        }
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT)
            return SendResult::Timeout;
        if (err == WSAECONNRESET || err == WSAECONNABORTED || err == WSAESHUTDOWN)
            return SendResult::Closed;
        return SendResult::Error;
    }
    return SendResult::Ok;
}

bool sendAll(SOCKET s, const char *data, int n)
{
    return sendAllEx(s, data, n) == SendResult::Ok;
}

// ---------------- Connecting to the destination ----------------

enum class ConnectResult
{
    Ok,
    Blocked,
    DnsFailed,
    ConnectFailed
};

SOCKET connectWithTimeout(const addrinfo *ai, int timeoutMs)
{
    SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;

    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == SOCKET_ERROR)
    {
        if (WSAGetLastError() != WSAEWOULDBLOCK)
        {
            closesocket(s);
            return INVALID_SOCKET;
        }
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval tv = toTimeval(timeoutMs);
        int r = select(0, nullptr, &writable, &failed, &tv);
        if (r <= 0 || FD_ISSET(s, &failed))
        {
            closesocket(s);
            return INVALID_SOCKET;
        }
        int err = 0;
        int len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
        if (err != 0)
        {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);
    applyIoTimeouts(s);
    return s;
}

// host may be "example.com", "1.2.3.4", or an IPv6 literal (brackets already stripped)
Socket connectTo(const std::string &host, const std::string &port, ConnectResult &result)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC; // try IPv6 and IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
    {
        result = ConnectResult::DnsFailed;
        return Socket();
    }

    if (!g_allowPrivate)
    {
        for (addrinfo *p = res; p; p = p->ai_next)
        {
            if (isBlockedAddress(p->ai_addr))
            {
                freeaddrinfo(res);
                result = ConnectResult::Blocked;
                return Socket();
            }
        }
    }

    result = ConnectResult::ConnectFailed;
    Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
    Socket s;
    for (addrinfo *p = res; p; p = p->ai_next)
    {
        int remaining = msUntil(deadline);
        if (remaining <= 0)
            break;
        SOCKET raw = connectWithTimeout(p, remaining);
        if (raw != INVALID_SOCKET)
        {
            s = Socket(raw);
            result = ConnectResult::Ok;
            break;
        }
    }
    freeaddrinfo(res);
    return s;
}

// ---------------- Relay (two-way copy with half-close and idle timeout) ----------------

enum class RelayEnd
{
    Closed,
    IdleTimeout,
    ClientGone,
    ServerGone,
    Error
};

const char *relayEndName(RelayEnd e)
{
    switch (e)
    {
    case RelayEnd::Closed:
        return "closed";
    case RelayEnd::IdleTimeout:
        return "idle_timeout";
    case RelayEnd::ClientGone:
        return "client_reset";
    case RelayEnd::ServerGone:
        return "server_reset";
    default:
        return "error";
    }
}

RelayEnd relay(SOCKET client, SOCKET server, long long &up, long long &down)
{
    char buf[16384];
    bool clientOpen = true;
    bool serverOpen = true;

    while (clientOpen || serverOpen)
    {
        fd_set readable;
        FD_ZERO(&readable);
        if (clientOpen)
            FD_SET(client, &readable);
        if (serverOpen)
            FD_SET(server, &readable);

        timeval tv = toTimeval(g_idleTimeoutMs);
        int r = select(0, &readable, nullptr, nullptr, &tv);
        if (r == 0)
            return RelayEnd::IdleTimeout;
        if (r < 0)
            return RelayEnd::Error;

        if (clientOpen && FD_ISSET(client, &readable))
        {
            int n = recv(client, buf, sizeof(buf), 0);
            if (n < 0)
                return RelayEnd::ClientGone;
            if (n == 0)
            {
                clientOpen = false;
                shutdown(server, SD_SEND);
            }
            else
            {
                SendResult sr = sendAllEx(server, buf, n);
                if (sr != SendResult::Ok)
                    return RelayEnd::ServerGone;
                up += n;
                onBytes(Direction::Up, n);
            }
        }

        if (serverOpen && FD_ISSET(server, &readable))
        {
            int n = recv(server, buf, sizeof(buf), 0);
            if (n < 0)
                return RelayEnd::ServerGone;
            if (n == 0)
            {
                serverOpen = false;
                shutdown(client, SD_SEND);
            }
            else
            {
                SendResult sr = sendAllEx(client, buf, n);
                if (sr != SendResult::Ok)
                    return RelayEnd::ClientGone;
                down += n;
                onBytes(Direction::Down, n);
            }
        }
    }
    return RelayEnd::Closed;
}

// ---------------- Parsing the request line / target ----------------

struct Target
{
    bool ok = false;
    bool isTunnel = false;
    std::string host;
    std::string port;
    std::string pathForRewrite; // only used for plain HTTP
};

// "www.google.com:443" or "[::1]:443" -> host + port
bool splitHostPort(const std::string &hostport, const std::string &defaultPort, std::string &host, std::string &port)
{
    if (!hostport.empty() && hostport[0] == '[')
    {
        size_t close = hostport.find(']');
        if (close == std::string::npos)
            return false;
        host = hostport.substr(1, close - 1);
        if (close + 1 < hostport.size() && hostport[close + 1] == ':')
            port = hostport.substr(close + 2);
        else
            port = defaultPort;
        return !host.empty();
    }
    size_t colon = hostport.rfind(':');
    // an IPv6 literal with no brackets and no port has more than one colon
    if (colon != std::string::npos && hostport.find(':') != colon)
    {
        host = hostport;
        port = defaultPort;
        return true;
    }
    host = hostport.substr(0, colon);
    port = (colon == std::string::npos) ? defaultPort : hostport.substr(colon + 1);
    return !host.empty();
}

Target parseTarget(const std::string &method, const std::string &target)
{
    Target t;
    t.isTunnel = (method == "CONNECT");

    if (t.isTunnel)
    {
        t.ok = splitHostPort(target, "443", t.host, t.port);
        return t;
    }

    if (target.rfind("http://", 0) != 0)
        return t;
    std::string rest = target.substr(7);
    size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    t.pathForRewrite = (slash == std::string::npos) ? "/" : rest.substr(slash);
    t.ok = splitHostPort(hostport, "80", t.host, t.port);
    return t;
}

// ---------------- Connection-end logging guard ----------------
//
// Whatever path a connection exits through (normal return, early reject,
// or an exception caught higher up), this prints exactly one summary line.

class ConnectionGuard
{
public:
    ConnectionGuard(std::string host, std::string port, bool isTunnel)
        : host_(std::move(host)), port_(std::move(port)), isTunnel_(isTunnel) {}

    ~ConnectionGuard()
    {
        if (reported_)
            return;
        logEvent(isTunnel_ ? "TUNNEL" : "HTTP",
                  cleanForLog(host_) + ":" + port_ +
                      "  up=" + humanBytes(up) +
                      "  down=" + humanBytes(down) +
                      "  end=" + reason_);
    }

    void finish(const std::string &reason)
    {
        reason_ = reason;
        reported_ = false; // let the destructor print it once
    }

    long long up = 0, down = 0;

private:
    std::string host_, port_;
    bool isTunnel_;
    std::string reason_ = "no_connection";
    bool reported_ = false;
};

// ---------------- Subscriber registry (skeleton) ----------------
//
// The map lock protects ONLY the lookup. Callers keep the shared_ptr and then
// touch the atomics without any lock, never during recv()/send().

struct Subscriber
{
    std::string msisdn;
    std::atomic<long long> balanceBytes{0};
    std::atomic<bool> unlimited5g{false};
    std::atomic<bool> on5g{false};
};

std::mutex g_subscribersMutex;
std::unordered_map<std::string, std::shared_ptr<Subscriber>> g_subscribers;

std::shared_ptr<Subscriber> findSubscriberByIp(const std::string &ip)
{
    // Next step: ask Redis for "ip:{ip}:msisdn", then find or create the entry.
    std::lock_guard<std::mutex> lock(g_subscribersMutex);
    auto it = g_subscribers.find(ip);
    return (it == g_subscribers.end()) ? nullptr : it->second;
}

// ---------------- One client connection ----------------

void handleClient(SOCKET rawClient, uint32_t clientIp)
{
    SlotGuard slot(clientIp);
    Socket client(rawClient);
    applyIoTimeouts(client.get());

    try
    {
        Head head;
        ReadResult rr = readHeaders(client.get(), head, kFirstRequestTimeoutMs);
        if (rr == ReadResult::TooLarge)
        {
            logEvent("REJECTED", "header too large");
            sendStatus(client.get(), "431 Request Header Fields Too Large");
            return;
        }
        if (rr == ReadResult::Timeout)
        {
            logEvent("REJECTED", "request timeout");
            sendStatus(client.get(), "408 Request Timeout");
            return;
        }
        if (rr != ReadResult::Ok)
            return; // client closed or errored before sending anything

        std::string firstLine = head.headers.substr(0, head.headers.find("\r\n"));
        size_t sp1 = firstLine.find(' ');
        size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : firstLine.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos)
        {
            sendStatus(client.get(), "400 Bad Request");
            return;
        }
        std::string method = firstLine.substr(0, sp1);
        std::string targetText = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);

        Target t = parseTarget(method, targetText);
        if (!t.ok)
        {
            sendStatus(client.get(), "400 Bad Request");
            return;
        }

        int portNum = 0;
        if (!parsePort(t.port, portNum))
        {
            sendStatus(client.get(), "400 Bad Request");
            return;
        }

        ConnectionGuard guard(t.host, t.port, t.isTunnel);

        if (!g_allowPrivate && !isAllowedPort(t.isTunnel, portNum))
        {
            logEvent("BLOCKED", cleanForLog(t.host) + ":" + t.port + " (port not allowed)");
            sendStatus(client.get(), "403 Forbidden");
            guard.finish("blocked_port");
            return;
        }

        ConnectResult cr = ConnectResult::ConnectFailed;
        Socket server = connectTo(t.host, t.port, cr);
        if (!server.valid())
        {
            bool blocked = (cr == ConnectResult::Blocked);
            logEvent(blocked ? "BLOCKED" : "FAILED", cleanForLog(t.host) + ":" + t.port);
            sendStatus(client.get(), blocked ? "403 Forbidden" : "502 Bad Gateway");
            guard.finish(blocked ? "blocked_destination" : "connect_failed");
            return;
        }

        if (t.isTunnel)
        {
            const char *ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
            if (sendAllEx(client.get(), ok, (int)strlen(ok)) != SendResult::Ok)
            {
                guard.finish("client_reset");
                return;
            }
            if (!head.leftover.empty())
            {
                if (sendAllEx(server.get(), head.leftover.data(), (int)head.leftover.size()) != SendResult::Ok)
                {
                    guard.finish("server_reset");
                    return;
                }
                guard.up += (long long)head.leftover.size();
                onBytes(Direction::Up, (long long)head.leftover.size());
            }
        }
        else
        {
            // Rewrite "GET http://host/path HTTP/1.1" -> "GET /path HTTP/1.1",
            // drop Proxy-* headers, and force the origin to close after one reply.
            std::vector<std::string> lines = headerLinesWithoutProxy(head.headers);
            std::string rewritten = method + " " + t.pathForRewrite + " HTTP/1.1\r\n";
            for (const auto &line : lines)
                rewritten += line + "\r\n";
            rewritten += "Connection: close\r\n\r\n";
            rewritten += head.leftover; // a request body, if any, travels unchanged

            if (sendAllEx(server.get(), rewritten.data(), (int)rewritten.size()) != SendResult::Ok)
            {
                guard.finish("server_reset");
                return;
            }
            guard.up = (long long)rewritten.size();
            onBytes(Direction::Up, guard.up);
        }

        RelayEnd end = relay(client.get(), server.get(), guard.up, guard.down);
        guard.finish(relayEndName(end));
    }
    catch (const std::exception &e)
    {
        logEvent("ERROR", std::string("connection: ") + e.what());
    }
    catch (...)
    {
        logEvent("ERROR", "connection: unknown");
    }
}

// ---------------- main ----------------

int main()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1;

    SetConsoleCtrlHandler(consoleHandler, TRUE);

    g_allowPrivate = (envOr("ALLOW_PRIVATE_DESTINATIONS", "0") == "1");

    std::string idleText = envOr("IDLE_TIMEOUT_SECONDS", "120");
    if (allDigits(idleText) && idleText.size() <= 5)
    {
        int sec = std::stoi(idleText);
        if (sec >= 1)
            g_idleTimeoutMs = sec * 1000;
    }

    g_allowedClients = parseCidrList(
        envOr("ALLOWED_CLIENTS", "192.168.0.0/16,10.0.0.0/8,127.0.0.1/32"));
    if (g_allowedClients.empty())
    {
        std::cerr << "ALLOWED_CLIENTS has no valid entries. Refusing to start.\n";
        return 1;
    }
    std::string bindIp = envOr("PROXY_BIND", "0.0.0.0");

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    if (inet_pton(AF_INET, bindIp.c_str(), &addr.sin_addr) != 1)
    {
        std::cerr << "PROXY_BIND is not a valid IPv4 address: " << bindIp << "\n";
        return 1;
    }

    if (bind(listener, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "bind() failed: " << WSAGetLastError() << "\n";
        return 1;
    }
    listen(listener, SOMAXCONN);
    logEvent("START", "listening on " + bindIp + ":8888  (idle timeout " +
                           std::to_string(g_idleTimeoutMs / 1000) + "s)");
    if (g_allowPrivate)
        logEvent("WARN", "ALLOW_PRIVATE_DESTINATIONS=1 (dev mode). Never leave this on.");

    while (!g_stopping)
    {
        // Wake up twice a second so Ctrl+C is noticed even when nobody connects.
        fd_set acceptSet;
        FD_ZERO(&acceptSet);
        FD_SET(listener, &acceptSet);
        timeval tv = toTimeval(500);
        int ready = select(0, &acceptSet, nullptr, nullptr, &tv);
        if (ready == 0)
            continue;
        if (ready < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        SOCKET client = accept(listener, (sockaddr *)&peer, &peerLen);
        if (client == INVALID_SOCKET)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        uint32_t ip = ntohl(peer.sin_addr.s_addr);
        char ipText[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, ipText, sizeof(ipText));

        if (!isClientAllowed(ip))
        {
            logEvent("DENIED", std::string("client ") + ipText);
            sendStatus(client, "403 Forbidden");
            closesocket(client);
            continue;
        }

        if (!acquireSlot(ip))
        {
            logEvent("REJECTED", std::string("too many connections from ") + ipText);
            sendStatus(client, "503 Service Unavailable");
            closesocket(client);
            continue;
        }

        g_activeThreads++;
        try
        {
            std::thread([client, ip]() {
                handleClient(client, ip);
                g_activeThreads--;
            }).detach();
        }
        catch (const std::exception &e)
        {
            logEvent("ERROR", std::string("could not start thread: ") + e.what());
            releaseSlot(ip);
            closesocket(client);
            g_activeThreads--;
        }
    }

    logEvent("STOP", "shutting down, waiting for " +
                          std::to_string(g_activeThreads.load()) + " active connection(s)...");
    closesocket(listener);
    for (int waited = 0; g_activeThreads > 0 && waited < 15000; waited += 200)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (g_activeThreads > 0)
        logEvent("STOP", std::to_string(g_activeThreads.load()) + " connection(s) still open, forcing exit");
    logEvent("STOP", "exiting");
    WSACleanup();
    return 0;
}