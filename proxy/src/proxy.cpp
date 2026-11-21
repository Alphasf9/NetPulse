#include <winsock2.h>
#include <ws2tcpip.h>
#include <hiredis/hiredis.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
        return TRUE;
    }
    return FALSE;
}

// ---------------- Settings ----------------

bool g_allowPrivate = false;
int g_idleTimeoutMs = 120000;

constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr int kFirstRequestTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSocketIoTimeoutMs = 60000;
constexpr int kMaxConnections = 512;
constexpr int kMaxPerClientIp = 256;
constexpr int kShutdownSoftWaitMs = 8000;
constexpr int kShutdownForceWaitMs = 5000;
constexpr long long kSubscriberCacheMs = 5000;
constexpr long long kSliceBytes = 1048576;       // 1 MB, matches slice.lua ARGV[1]
constexpr long long kEmptyCooldownMs = 1000;     // while balance is 0, re-check Redis at most this often
constexpr long long kThrottleBytesPerSec = 8192; // 64 kbps, per your spec

struct Cidr
{
    uint32_t base;
    uint32_t mask;
};
std::vector<Cidr> g_allowedClients;

// ---------------- Logging ----------------

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

std::string tag(const std::string &t)
{
    std::string out = t;
    while (out.size() < 11)
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

std::string humanBytes(long long n)
{
    char buf[32];
    if (n < 1024)
        snprintf(buf, sizeof(buf), "%lld B", n);
    else if (n < 1024LL * 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024.0));
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

bool startsWithCI(const std::string &line, const std::string &prefix)
{
    if (line.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); i++)
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)prefix[i]))
            return false;
    return true;
}

std::string toLowerCopy(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        c = (char)tolower((unsigned char)c);
    return out;
}

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------- sendAll with a real reason ----------------

enum class SendResult
{
    Ok,
    Closed,
    Timeout,
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

void sendStatus(SOCKET s, const char *status)
{
    std::string r = std::string("HTTP/1.1 ") + status +
                    "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if (sendAllEx(s, r.c_str(), (int)r.size()) != SendResult::Ok)
        logEvent("WARN", std::string("could not fully send status '") + status + "' to client");
}

// ---------------- Accounting hook ----------------

enum class Direction
{
    Up,
    Down
};

void onBytes(Direction /*dir*/, long long /*n*/)
{
    // placeholder: quota logic lives in checkQuota() below, not here
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
    DWORD t = kSocketIoTimeoutMs;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t)) == SOCKET_ERROR)
        logEvent("WARN", "could not set receive timeout on a socket");
    if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t)) == SOCKET_ERROR)
        logEvent("WARN", "could not set send timeout on a socket");
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

// ---------------- Active-socket registry (for shutdown) ----------------

std::mutex g_activeSocketsMutex;
std::unordered_set<SOCKET> g_activeSockets;

void registerActiveSocket(SOCKET s)
{
    std::lock_guard<std::mutex> lock(g_activeSocketsMutex);
    g_activeSockets.insert(s);
}

void unregisterActiveSocket(SOCKET s)
{
    std::lock_guard<std::mutex> lock(g_activeSocketsMutex);
    g_activeSockets.erase(s);
}

void forceShutdownActiveSockets()
{
    std::lock_guard<std::mutex> lock(g_activeSocketsMutex);
    for (SOCKET s : g_activeSockets)
        shutdown(s, SD_BOTH);
}

class SocketRegistration
{
public:
    explicit SocketRegistration(SOCKET s) : s_(s) { registerActiveSocket(s_); }
    ~SocketRegistration() { unregisterActiveSocket(s_); }
    SocketRegistration(const SocketRegistration &) = delete;
    SocketRegistration &operator=(const SocketRegistration &) = delete;

private:
    SOCKET s_;
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
        {0x00000000, 8}, {0x0A000000, 8}, {0x64400000, 10}, {0x7F000000, 8}, {0xA9FE0000, 16}, {0xAC100000, 12}, {0xC0000000, 24}, {0xC0000200, 24}, {0xC0A80000, 16}, {0xC6120000, 15}, {0xC6336400, 24}, {0xCB007100, 24}, {0xE0000000, 4}, {0xF0000000, 4}};
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
            return true;
        if ((b[0] & 0xFE) == 0xFC)
            return true;
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80)
            return true;
        if (b[0] == 0xFF)
            return true;
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0D && b[3] == 0xB8)
            return true;
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
    std::string headers;
    std::string leftover;
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

// ---------------- Header rewriting ----------------

bool isAlwaysHopByHop(const std::string &lowerName)
{
    static const char *names[] = {
        "connection", "keep-alive", "proxy-authenticate",
        "proxy-authorization", "te", "trailer", "trailers", "upgrade"};
    for (const char *n : names)
        if (lowerName == n)
            return true;
    return startsWithCI(lowerName, "proxy-");
}

std::vector<std::string> splitCommaList(const std::string &value)
{
    std::vector<std::string> out;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        if (b != std::string::npos)
            out.push_back(item.substr(b, e - b + 1));
    }
    return out;
}

bool splitHeaderLine(const std::string &line, std::string &lowerName, std::string &value)
{
    size_t colon = line.find(':');
    if (colon == std::string::npos)
        return false;
    lowerName = toLowerCopy(line.substr(0, colon));
    size_t vstart = colon + 1;
    while (vstart < line.size() && (line[vstart] == ' ' || line[vstart] == '\t'))
        vstart++;
    value = line.substr(vstart);
    return true;
}

std::vector<std::string> headerLinesForForwarding(const std::string &headers)
{
    std::vector<std::string> rawLines;
    size_t pos = headers.find("\r\n");
    pos += 2;
    while (pos < headers.size())
    {
        size_t next = headers.find("\r\n", pos);
        if (next == std::string::npos)
            break;
        std::string line = headers.substr(pos, next - pos);
        pos = next + 2;
        if (line.empty())
            break;
        rawLines.push_back(line);
    }

    std::vector<std::string> extraHopByHop;
    for (const auto &line : rawLines)
    {
        std::string name, value;
        if (splitHeaderLine(line, name, value) && name == "connection")
            for (const auto &nominated : splitCommaList(value))
                extraHopByHop.push_back(toLowerCopy(nominated));
    }

    std::vector<std::string> out;
    for (const auto &line : rawLines)
    {
        std::string name, value;
        if (!splitHeaderLine(line, name, value))
        {
            out.push_back(line);
            continue;
        }
        if (isAlwaysHopByHop(name))
            continue;
        bool nominated = false;
        for (const auto &extra : extraHopByHop)
            if (extra == name)
            {
                nominated = true;
                break;
            }
        if (nominated)
            continue;
        out.push_back(line);
    }
    return out;
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
    if (ioctlsocket(s, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }

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
    if (ioctlsocket(s, FIONBIO, &blocking) == SOCKET_ERROR)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    applyIoTimeouts(s);
    return s;
}

Socket connectTo(const std::string &host, const std::string &port, ConnectResult &result)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
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

// ---------------- Redis connection (RAII, one per thread) ----------------

class RedisConn
{
public:
    ~RedisConn() { reset(); }

    redisContext *get()
    {
        if (ctx_ == nullptr)
            connect();
        return ctx_;
    }

    void dropAndReconnectNextTime()
    {
        reset();
    }

private:
    void connect()
    {
        ctx_ = redisConnect("127.0.0.1", 6379);
        if (ctx_ == nullptr || ctx_->err)
        {
            logEvent("ERROR", std::string("Redis connect failed: ") +
                                  (ctx_ ? ctx_->errstr : "allocation failed"));
            reset();
        }
    }

    void reset()
    {
        if (ctx_ != nullptr)
        {
            redisFree(ctx_);
            ctx_ = nullptr;
        }
    }

    redisContext *ctx_ = nullptr;
};

thread_local RedisConn t_redis;

// ---------------- Slice script loading ----------------

std::string g_sliceScript;

bool loadSliceScript()
{
    std::ifstream f("..\\..\\redis\\slice.lua");
    if (!f.is_open())
    {
        logEvent("ERROR", "could not open ../../redis/slice.lua (check working directory)");
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    g_sliceScript = ss.str();
    return !g_sliceScript.empty();
}

// ---------------- Live event publishing (Pub/Sub, for the dashboard) ----------------

void publishEvent(const std::string &json)
{
    redisContext *rc = t_redis.get();
    if (rc == nullptr)
        return; // best-effort — a missed dashboard event never affects traffic
    redisReply *r = (redisReply *)redisCommand(rc, "PUBLISH telecom:telemetry %s", json.c_str());
    if (r)
        freeReplyObject(r);
}

std::string jsonEscape(const std::string &s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

// ---------------- Subscriber registry ----------------

struct Subscriber
{
    std::string msisdn;
    std::atomic<long long> balanceBytes{0};
    std::atomic<bool> unlimited5g{false};
    std::atomic<bool> on5g{false};
    std::atomic<long long> lastRefreshedMs{0};
    std::atomic<long long> sliceRemaining{0};
    std::atomic<long long> emptyUntilMs{0};
    std::atomic<bool> throttled{false}; // true => enforce the 64 kbps cap on downloads
};

std::mutex g_subscribersMutex;
std::unordered_map<std::string, std::shared_ptr<Subscriber>> g_subscribers;

bool refreshSubscriberFromRedis(redisContext *rc, Subscriber &sub)
{
    redisReply *balReply = (redisReply *)redisCommand(rc, "GET user:%s:balance_bytes", sub.msisdn.c_str());
    if (balReply == nullptr)
        return false;
    if (balReply->type == REDIS_REPLY_STRING)
        sub.balanceBytes = std::atoll(balReply->str);
    freeReplyObject(balReply);

    redisReply *unlReply = (redisReply *)redisCommand(rc, "GET user:%s:unlimited_5g", sub.msisdn.c_str());
    if (unlReply == nullptr)
        return false;
    sub.unlimited5g = (unlReply->type == REDIS_REPLY_STRING && std::string(unlReply->str) == "1");
    freeReplyObject(unlReply);

    redisReply *ratReply = (redisReply *)redisCommand(rc, "GET user:%s:rat_type", sub.msisdn.c_str());
    if (ratReply == nullptr)
        return false;
    sub.on5g = (ratReply->type == REDIS_REPLY_STRING && std::string(ratReply->str) == "5G_NR");
    freeReplyObject(ratReply);

    sub.lastRefreshedMs = nowMs();

    // A recharge noticed here (balance now positive, or 5G-unlimited) clears
    // both the Redis-call cooldown AND the active throttle immediately,
    // rather than waiting for the subscriber's next slice exhaustion event.
    if (sub.balanceBytes.load() > 0 || sub.unlimited5g.load())
    {
        sub.emptyUntilMs = 0;
        sub.throttled = false;
    }

    return true;
}

std::shared_ptr<Subscriber> findSubscriberByIp(const std::string &ip)
{
    std::shared_ptr<Subscriber> cached;
    {
        std::lock_guard<std::mutex> lock(g_subscribersMutex);
        auto it = g_subscribers.find(ip);
        if (it != g_subscribers.end())
            cached = it->second;
    }

    if (cached && (nowMs() - cached->lastRefreshedMs.load()) < kSubscriberCacheMs)
        return cached;

    redisContext *rc = t_redis.get();
    if (rc == nullptr)
        return cached;

    if (cached)
    {
        if (!refreshSubscriberFromRedis(rc, *cached))
            t_redis.dropAndReconnectNextTime();
        return cached;
    }

    redisReply *reply = (redisReply *)redisCommand(rc, "GET ip:%s:msisdn", ip.c_str());
    if (reply == nullptr)
    {
        t_redis.dropAndReconnectNextTime();
        return nullptr;
    }
    if (reply->type != REDIS_REPLY_STRING)
    {
        freeReplyObject(reply);
        return nullptr;
    }
    std::string msisdn = reply->str;
    freeReplyObject(reply);

    auto sub = std::make_shared<Subscriber>();
    sub->msisdn = msisdn;
    if (!refreshSubscriberFromRedis(rc, *sub))
    {
        t_redis.dropAndReconnectNextTime();
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_subscribersMutex);
        g_subscribers[ip] = sub;
    }
    return sub;
}

// ---------------- Quota checking + enforcement ----------------
//
// Spends from a local RAM counter (sliceRemaining) first, and only calls
// Redis (slice.lua) when that runs out ("write-behind slicing"). On
// WouldThrottle, sub->throttled is set to true — relay() reads that flag
// on the DOWNLOAD path and actually paces sends to ~64 kbps (per your
// spec: sleep_for, never severing the connection). Upload is NOT paced;
// it is still logged only — see the design note in relay().

enum class QuotaDecision
{
    AllowedLocal,
    AllowedFresh,
    WouldThrottle,
    NoSubscriber,
    RedisError
};

QuotaDecision checkQuota(const std::shared_ptr<Subscriber> &sub, long long n, bool isUpload)
{
    if (!sub)
        return QuotaDecision::NoSubscriber;

    if (sub->sliceRemaining.load() >= n)
    {
        sub->sliceRemaining -= n;
        return QuotaDecision::AllowedLocal;
    }

    long long now = nowMs();
    if (now < sub->emptyUntilMs.load())
    {
        sub->throttled = true; // still in the cooldown window: still empty
        return QuotaDecision::WouldThrottle;
    }

    redisContext *rc = t_redis.get();
    if (rc == nullptr)
        return QuotaDecision::RedisError; // fail OPEN: an infra hiccup never cuts off traffic

    std::string balKey = "user:" + sub->msisdn + ":balance_bytes";
    std::string ratKey = "user:" + sub->msisdn + ":rat_type";
    std::string unlKey = "user:" + sub->msisdn + ":unlimited_5g";
    long long previousBalance = sub->balanceBytes.load();
    bool wasThrottled = sub->throttled.load();

    redisReply *reply = (redisReply *)redisCommand(
        rc, "EVAL %s 3 %s %s %s %lld",
        g_sliceScript.c_str(), balKey.c_str(), ratKey.c_str(), unlKey.c_str(), kSliceBytes);

    if (reply == nullptr)
    {
        t_redis.dropAndReconnectNextTime();
        return QuotaDecision::RedisError;
    }
    if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2)
    {
        logEvent("ERROR", "unexpected slice.lua reply shape");
        freeReplyObject(reply);
        return QuotaDecision::RedisError;
    }

    long long granted = reply->element[0]->integer;
    long long newBalance = reply->element[1]->integer;
    freeReplyObject(reply);

    if (granted == 0)
    {
        sub->emptyUntilMs = now + kEmptyCooldownMs;
        bool firstExhaustion = !wasThrottled;
        sub->throttled = true;
        sub->balanceBytes = newBalance;

        if (firstExhaustion)
        {
            publishEvent("{\"type\":\"QUOTA_EXHAUSTED\",\"msisdn\":\"" + sub->msisdn +
                         "\",\"ts\":" + std::to_string(now) + "}");
            publishEvent("{\"type\":\"THROTTLE_STARTED\",\"msisdn\":\"" + sub->msisdn +
                         "\",\"ts\":" + std::to_string(now) + "}");
        }
        return QuotaDecision::WouldThrottle;
    }

    sub->emptyUntilMs = 0;
    sub->throttled = false; // a fresh grant (recharge, or 5G zero-rating) lifts the throttle
    sub->sliceRemaining = granted - n;
    sub->balanceBytes = newBalance;

    publishEvent("{\"type\":\"QUOTA_UPDATE\",\"msisdn\":\"" + sub->msisdn +
                 "\",\"previousBalance\":" + std::to_string(previousBalance) +
                 ",\"deducted\":" + std::to_string(previousBalance - newBalance) +
                 ",\"newBalance\":" + std::to_string(newBalance) +
                 ",\"direction\":\"" + (isUpload ? "upload" : "download") + "\"" +
                 ",\"ts\":" + std::to_string(now) + "}");

    if (wasThrottled)
        publishEvent("{\"type\":\"THROTTLE_STOPPED\",\"msisdn\":\"" + sub->msisdn +
                     "\",\"ts\":" + std::to_string(now) + "}");

    return QuotaDecision::AllowedFresh;
}

// ---------------- Relay ----------------

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

// Sends `data` (n bytes) to `dest` in ~1-second-worth chunks, sleeping
// between chunks so the achieved rate is capped at kThrottleBytesPerSec.
// Per your spec: this throttles, it does NOT drop the connection.
SendResult sendThrottled(SOCKET dest, const char *data, int n)
{
    int sent = 0;
    while (sent < n)
    {
        int chunk = (int)std::min<long long>(kThrottleBytesPerSec, n - sent);
        Clock::time_point chunkStart = Clock::now();

        SendResult r = sendAllEx(dest, data + sent, chunk);
        if (r != SendResult::Ok)
            return r;
        sent += chunk;

        if (sent < n)
        {
            auto elapsed = Clock::now() - chunkStart;
            auto target = std::chrono::milliseconds(1000);
            if (elapsed < target)
                std::this_thread::sleep_for(target - elapsed);
        }
    }
    return SendResult::Ok;
}

// sub is needed so this can check quota on both directions and enforce the
// download throttle.
RelayEnd relay(SOCKET client, SOCKET server,
               long long &up, long long &down,
               const std::shared_ptr<Subscriber> &sub)
{
    char buf[16384];
    bool clientOpen = true;
    bool serverOpen = true;

    bool throttleLoggedUp = false;
    bool throttleLoggedDown = false;

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

        // =========================================
        // UPLOAD: Tablet -> Internet Server
        // (log-only — not paced; see design note below)
        // =========================================
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
                if (sendAllEx(server, buf, n) != SendResult::Ok)
                    return RelayEnd::ServerGone;

                up += n;
                onBytes(Direction::Up, n);

                QuotaDecision qd = checkQuota(sub, n, /*isUpload=*/true);
                if (qd == QuotaDecision::WouldThrottle)
                {
                    if (!throttleLoggedUp)
                    {
                        logEvent("QUOTA", sub->msisdn +
                                              " THROTTLED (upload logged only, not paced), after " +
                                              humanBytes(up) + " this connection");
                        throttleLoggedUp = true;
                    }
                }
                else if (qd == QuotaDecision::AllowedFresh)
                {
                    logEvent("QUOTA", sub->msisdn +
                                          " fresh slice granted (upload), balance now " +
                                          humanBytes(sub->balanceBytes.load()));
                    throttleLoggedUp = false;
                }
            }
        }

        // =========================================
        // DOWNLOAD: Internet Server -> Tablet
        // (ENFORCED — paced to 64 kbps once throttled)
        // =========================================
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
                // Check quota BEFORE sending this chunk, so the pacing
                // decision for THIS chunk is based on current state.
                QuotaDecision qd = checkQuota(sub, n, /*isUpload=*/false);

                bool paceThisChunk = (qd == QuotaDecision::WouldThrottle);

                SendResult sr = paceThisChunk
                                    ? sendThrottled(client, buf, n)
                                    : sendAllEx(client, buf, n);

                if (sr != SendResult::Ok)
                    return RelayEnd::ClientGone;

                down += n;
                onBytes(Direction::Down, n);

                if (qd == QuotaDecision::WouldThrottle)
                {
                    if (!throttleLoggedDown)
                    {
                        logEvent("QUOTA", sub->msisdn +
                                              " THROTTLED to 64 kbps (download), after " +
                                              humanBytes(down) + " this connection");
                        throttleLoggedDown = true;
                    }
                }
                else if (qd == QuotaDecision::AllowedFresh)
                {
                    logEvent("QUOTA", sub->msisdn +
                                          " fresh slice granted (download), full speed restored, balance now " +
                                          humanBytes(sub->balanceBytes.load()));
                    throttleLoggedDown = false;
                }
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
    std::string pathForRewrite;
};

bool looksLikeValidHost(const std::string &host)
{
    if (host.empty())
        return false;
    for (char c : host)
        if ((unsigned char)c <= 0x20 || c == '/' || c == '\\')
            return false;
    return true;
}

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
        t.ok = splitHostPort(target, "443", t.host, t.port) && looksLikeValidHost(t.host);
        return t;
    }

    if (!startsWithCI(target, "http://"))
        return t;

    std::string rest = target.substr(7);
    if (rest.empty())
        return t;

    size_t firstDelim = rest.find_first_of("/?#");
    std::string authorityCandidate = (firstDelim == std::string::npos) ? rest : rest.substr(0, firstDelim);

    if (authorityCandidate.find('@') != std::string::npos)
        return t;

    std::string pathAndQuery;
    if (firstDelim == std::string::npos)
        pathAndQuery = "/";
    else if (rest[firstDelim] == '/')
        pathAndQuery = rest.substr(firstDelim);
    else
        pathAndQuery = "/" + rest.substr(firstDelim);

    size_t hashPos = pathAndQuery.find('#');
    if (hashPos != std::string::npos)
        pathAndQuery = pathAndQuery.substr(0, hashPos);

    t.pathForRewrite = pathAndQuery;
    t.ok = splitHostPort(authorityCandidate, "80", t.host, t.port) && looksLikeValidHost(t.host);
    return t;
}

// ---------------- Connection-end logging guard ----------------

class ConnectionGuard
{
public:
    ConnectionGuard(std::string host, std::string port, bool isTunnel, std::shared_ptr<Subscriber> sub)
        : host_(std::move(host)), port_(std::move(port)), isTunnel_(isTunnel), sub_(std::move(sub)) {}

    ~ConnectionGuard()
    {
        std::string line = cleanForLog(host_) + ":" + port_ +
                           "  up=" + humanBytes(up) +
                           "  down=" + humanBytes(down) +
                           "  end=" + reason_;
        logEvent(isTunnel_ ? "TUNNEL" : "HTTP", line);

        if (sub_)
        {
            redisContext *rc = t_redis.get();
            if (rc != nullptr)
            {
                std::string json = "{\"msisdn\":\"" + sub_->msisdn + "\","
                                                                     "\"rat\":\"" +
                                   (sub_->on5g.load() ? "5G_NR" : "4G_LTE") + "\","
                                                                              "\"up\":" +
                                   std::to_string(up) + ","
                                                        "\"down\":" +
                                   std::to_string(down) + ","
                                                          "\"host\":\"" +
                                   cleanForLog(host_) + "\","
                                                        "\"end\":\"" +
                                   reason_ + "\"}";
                redisReply *r = (redisReply *)redisCommand(rc, "LPUSH cdr:queue %s", json.c_str());
                if (r)
                    freeReplyObject(r);
            }

            publishEvent("{\"type\":\"NETWORK_ACTIVITY\",\"msisdn\":\"" + sub_->msisdn +
                         "\",\"kind\":\"" + (isTunnel_ ? "TUNNEL" : "HTTP") +
                         "\",\"host\":\"" + jsonEscape(cleanForLog(host_)) +
                         "\",\"up\":" + std::to_string(up) +
                         ",\"down\":" + std::to_string(down) +
                         ",\"endReason\":\"" + reason_ + "\"" +
                         ",\"ts\":" + std::to_string(nowMs()) + "}");
        }
    }

    void finish(const std::string &reason) { reason_ = reason; }

    long long up = 0, down = 0;

private:
    std::string host_, port_;
    bool isTunnel_;
    std::shared_ptr<Subscriber> sub_;
    std::string reason_ = "no_connection";
};

// ---------------- Managed worker threads ----------------

struct Worker
{
    std::thread t;
    std::shared_ptr<std::atomic<bool>> done;
};

std::mutex g_workersMutex;
std::vector<Worker> g_workers;

void pruneFinishedWorkers()
{
    std::lock_guard<std::mutex> lock(g_workersMutex);
    for (size_t i = 0; i < g_workers.size();)
    {
        if (g_workers[i].done->load())
        {
            if (g_workers[i].t.joinable())
                g_workers[i].t.join();
            g_workers[i] = std::move(g_workers.back());
            g_workers.pop_back();
        }
        else
        {
            i++;
        }
    }
}

void joinAllWorkersBlocking()
{
    std::lock_guard<std::mutex> lock(g_workersMutex);
    for (auto &w : g_workers)
        if (w.t.joinable())
            w.t.join();
    g_workers.clear();
}

// ---------------- One client connection ----------------

void handleClient(SOCKET rawClient, uint32_t clientIp, std::string clientIpText)
{
    SlotGuard slot(clientIp);
    Socket client(rawClient);
    SocketRegistration clientReg(client.get());
    applyIoTimeouts(client.get());

    auto sub = findSubscriberByIp(clientIpText);
    if (sub)
    {
        publishEvent("{\"type\":\"SUBSCRIBER_CONNECTED\",\"msisdn\":\"" + sub->msisdn +
                     "\",\"balance\":" + std::to_string(sub->balanceBytes.load()) +
                     ",\"unlimited5g\":" + (sub->unlimited5g.load() ? "true" : "false") +
                     ",\"on5g\":" + (sub->on5g.load() ? "true" : "false") +
                     ",\"ts\":" + std::to_string(nowMs()) + "}");
        logEvent("SUBSCRIBER", sub->msisdn + " balance=" + humanBytes(sub->balanceBytes.load()) +
                                   " 5g_plan=" + (sub->unlimited5g.load() ? "yes" : "no") +
                                   " on_5g=" + (sub->on5g.load() ? "yes" : "no") +
                                   " throttled=" + (sub->throttled.load() ? "yes" : "no"));
    }
    else
    {
        logEvent("SUBSCRIBER", "no binding for " + clientIpText);
    }

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
            return;

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

        ConnectionGuard guard(t.host, t.port, t.isTunnel, sub);

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
        SocketRegistration serverReg(server.get());

        long long originalRequestBytes = (long long)(head.headers.size() + head.leftover.size());
        guard.up = originalRequestBytes;
        onBytes(Direction::Up, originalRequestBytes);

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
            }
        }
        else
        {
            std::vector<std::string> lines = headerLinesForForwarding(head.headers);
            std::string rewritten = method + " " + t.pathForRewrite + " HTTP/1.1\r\n";
            for (const auto &line : lines)
                rewritten += line + "\r\n";
            rewritten += "Connection: close\r\n\r\n";
            rewritten += head.leftover;

            if (sendAllEx(server.get(), rewritten.data(), (int)rewritten.size()) != SendResult::Ok)
            {
                guard.finish("server_reset");
                return;
            }
        }

        RelayEnd end = relay(client.get(), server.get(), guard.up, guard.down, sub);
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

    if (!loadSliceScript())
    {
        std::cerr << "Could not load slice.lua — quota checks will fail.\n";
        WSACleanup();
        return 1;
    }

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
        WSACleanup();
        return 1;
    }
    std::string bindIp = envOr("PROXY_BIND", "0.0.0.0");

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
    {
        std::cerr << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    if (inet_pton(AF_INET, bindIp.c_str(), &addr.sin_addr) != 1)
    {
        std::cerr << "PROXY_BIND is not a valid IPv4 address: " << bindIp << "\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    if (bind(listener, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "bind() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    if (listen(listener, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    logEvent("START", "listening on " + bindIp + ":8888  (idle timeout " +
                          std::to_string(g_idleTimeoutMs / 1000) + "s)");
    if (g_allowPrivate)
        logEvent("WARN", "ALLOW_PRIVATE_DESTINATIONS=1 (dev mode). Never leave this on.");

    while (!g_stopping)
    {
        pruneFinishedWorkers();

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
        std::string ipTextStr(ipText);

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

        auto done = std::make_shared<std::atomic<bool>>(false);
        g_activeThreads++;
        try
        {
            std::thread t([client, ip, ipTextStr, done]()
                          {
                handleClient(client, ip, ipTextStr);
                done->store(true);
                g_activeThreads--; });
            std::lock_guard<std::mutex> lock(g_workersMutex);
            g_workers.push_back(Worker{std::move(t), done});
        }
        catch (const std::exception &e)
        {
            logEvent("ERROR", std::string("could not start thread: ") + e.what());
            releaseSlot(ip);
            closesocket(client);
            g_activeThreads--;
        }
    }

    logEvent("STOP", "no longer accepting new connections");
    closesocket(listener);

    for (int waited = 0; g_activeThreads > 0 && waited < kShutdownSoftWaitMs; waited += 200)
    {
        pruneFinishedWorkers();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    pruneFinishedWorkers();

    if (g_activeThreads > 0)
    {
        logEvent("STOP", std::to_string(g_activeThreads.load()) +
                             " connection(s) still active; shutting down their sockets");
        forceShutdownActiveSockets();

        for (int waited = 0; g_activeThreads > 0 && waited < kShutdownForceWaitMs; waited += 200)
        {
            pruneFinishedWorkers();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        pruneFinishedWorkers();
    }

    logEvent("STOP", "joining remaining worker threads...");
    joinAllWorkersBlocking();

    logEvent("STOP", "exiting");
    WSACleanup();
    return 0;
}