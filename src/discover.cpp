/**
 * DISCOVER.CPP - SSDP DISCOVERY ENGINE
 */
#include "discover.h"
#include <iostream>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>
#include <ctime>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#endif

static const char *SSDP_MULTICAST_GROUP = "239.255.255.250";
static const int SSDP_MULTICAST_PORT = 1900;
static const int SOCKET_BUFFER_SIZE = 8 * 1024 * 1024; // 8MB socket buffer

#ifdef _WIN32
typedef SOCKET disc_sock_t;
#define DISC_INVALID INVALID_SOCKET
#define DISC_CLOSE(s) closesocket(s)
#else
typedef int disc_sock_t;
#define DISC_INVALID (-1)
#define DISC_CLOSE(s) close(s)
#endif

/**
 * All usable local IPv4 addresses (skips loopback + link-local).
 */
std::vector<std::string> listLocalIPv4()
{
    std::vector<std::string> out;
#ifdef _WIN32
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<char> buf(size);
    IP_ADAPTER_ADDRESSES *addrs = (IP_ADAPTER_ADDRESSES *)buf.data();
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (ret == ERROR_BUFFER_OVERFLOW)
    {
        buf.resize(size);
        addrs = (IP_ADAPTER_ADDRESSES *)buf.data();
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }
    if (ret != NO_ERROR)
        return out;
    for (IP_ADAPTER_ADDRESSES *a = addrs; a; a = a->Next)
    {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
        {
            if (u->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            char ipbuf[INET_ADDRSTRLEN] = {};
            const struct sockaddr_in *sin = (const struct sockaddr_in *)u->Address.lpSockaddr;
            if (!inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf)))
                continue;
            std::string ip = ipbuf;
            if (ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0)
                continue;
            out.push_back(ip);
        }
    }
#else
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) != 0)
        return out;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
        char ipbuf[INET_ADDRSTRLEN] = {};
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ifa->ifa_addr;
        if (!inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf)))
            continue;
        std::string ip = ipbuf;
        if (ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0)
            continue;
        out.push_back(ip);
    }
    freeifaddrs(ifaddr);
#endif
    return out;
}

/* ip + on-link prefix length pairs */
struct LocalIface
{
    std::string ip;
    int prefix;
};

static std::vector<LocalIface> listLocalIPv4Ifaces()
{
    std::vector<LocalIface> out;
#ifdef _WIN32
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<char> buf(size);
    IP_ADAPTER_ADDRESSES *addrs = (IP_ADAPTER_ADDRESSES *)buf.data();
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (ret == ERROR_BUFFER_OVERFLOW)
    {
        buf.resize(size);
        addrs = (IP_ADAPTER_ADDRESSES *)buf.data();
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }
    if (ret != NO_ERROR)
        return out;
    for (IP_ADAPTER_ADDRESSES *a = addrs; a; a = a->Next)
    {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
        {
            if (u->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            char ipbuf[INET_ADDRSTRLEN] = {};
            const struct sockaddr_in *sin = (const struct sockaddr_in *)u->Address.lpSockaddr;
            if (!inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf)))
                continue;
            std::string ip = ipbuf;
            if (ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0)
                continue;
            out.push_back({ip, (int)u->OnLinkPrefixLength});
        }
    }
#else
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) != 0)
        return out;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
        char ipbuf[INET_ADDRSTRLEN] = {};
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ifa->ifa_addr;
        if (!inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf)))
            continue;
        std::string ip = ipbuf;
        if (ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0)
            continue;
        int prefix = 32;
        if (ifa->ifa_netmask && ifa->ifa_netmask->sa_family == AF_INET)
        {
            uint32_t mask = ntohl(((const struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr);
            prefix = 0;
            while (mask & 0x80000000u) { prefix++; mask <<= 1; }
        }
        out.push_back({ip, prefix});
    }
    freeifaddrs(ifaddr);
#endif
    return out;
}

/**
 * Local IP of the interface facing peer_ip: prefers an on-link address in
 * the same subnet, then the kernel route lookup. VPN/TUN proxies often
 * hijack the route table, so the on-link check comes first.
 */
std::string getLocalIPForPeer(const std::string &peer_ip)
{
    struct in_addr peer_addr;
    if (peer_ip.empty() || inet_pton(AF_INET, peer_ip.c_str(), &peer_addr) != 1)
        return "";

    uint32_t peer = ntohl(peer_addr.s_addr);
    for (const LocalIface &iface : listLocalIPv4Ifaces())
    {
        if (iface.prefix <= 0 || iface.prefix > 32)
            continue;
        struct in_addr a;
        if (inet_pton(AF_INET, iface.ip.c_str(), &a) != 1)
            continue;
        uint32_t local = ntohl(a.s_addr);
        uint32_t mask = iface.prefix == 32 ? 0xFFFFFFFFu : ~((1u << (32 - iface.prefix)) - 1);
        if ((local & mask) == (peer & mask))
            return iface.ip;
    }

    /* Fallback: UDP connect() performs only a route lookup — no packet sent */
    struct sockaddr_in peer_sa = {};
    peer_sa.sin_family = AF_INET;
    peer_sa.sin_addr = peer_addr;
    peer_sa.sin_port = htons(9); /* arbitrary; never contacted */

#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET)
        return "";
    if (connect(sock, (struct sockaddr *)&peer_sa, sizeof(peer_sa)) == SOCKET_ERROR)
    {
        closesocket(sock);
        return "";
    }
    struct sockaddr_in local = {};
    int len = sizeof(local);
    if (getsockname(sock, (struct sockaddr *)&local, &len) == SOCKET_ERROR)
    {
        closesocket(sock);
        return "";
    }
    closesocket(sock);
#else
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return "";
    if (connect(sock, (struct sockaddr *)&peer_sa, sizeof(peer_sa)) < 0)
    {
        close(sock);
        return "";
    }
    struct sockaddr_in local = {};
    socklen_t len = sizeof(local);
    if (getsockname(sock, (struct sockaddr *)&local, &len) < 0)
    {
        close(sock);
        return "";
    }
    close(sock);
#endif

    char buf[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)))
        return "";
    return std::string(buf);
}

/**
 * Initialize sockets (Windows only)
 */
bool initSockets()
{
#ifdef _WIN32
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        std::cerr << "X. WSAStartup failed: " << result << std::endl;
        return false;
    }
#endif
    return true;
}

/**
 * Cleanup sockets (Windows only)
 */
void cleanupSockets()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

/**
 * Get the local IP address of this machine.
 * Prefers real LAN addresses over tunnel/VPN adapter ranges (some TUN adapters use 198.18.x).
 */
std::string getLocalIPAddress()
{
    std::vector<std::string> ips = listLocalIPv4();
    std::string best;
    int best_score = -999;
    for (const std::string &ip : ips)
    {
        int score = 0;
        if (ip.rfind("10.", 0) == 0) score = 3;
        else if (ip.rfind("192.168.", 0) == 0) score = 3;
        else if (ip.rfind("172.", 0) == 0) score = 2;
        else if (ip.rfind("198.18.", 0) == 0 || ip.rfind("198.19.", 0) == 0 ||
                 ip.rfind("100.64.", 0) == 0 || ip.rfind("100.65.", 0) == 0) score = -3;
        else score = 1;
        if (score > best_score) { best_score = score; best = ip; }
    }
    if (!best.empty()) return best;
    return "127.0.0.1";
}

/**
 * Test if a TCP connection can be established to a receiver
 */
bool testTcpConnection(const std::string &ip, int port, int timeout_ms)
{
    if (port < 1 || port > 65535 || timeout_ms < 0) return false;
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return false;
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 || sock >= FD_SETSIZE) {
        if (sock >= 0) close(sock);
        return false;
    }
#endif
    const auto fail = [&] { DISC_CLOSE(sock); return false; };
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) return fail();
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return fail();
#endif
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return fail();

    const int result = connect(sock, (sockaddr *)&addr, sizeof(addr));
    bool connected = (result == 0);
    bool pending = false;
    if (result != 0) {
#ifdef _WIN32
        const int err = WSAGetLastError();
        pending = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
        pending = (errno == EINPROGRESS);
#endif
    }
    if (pending) {
        fd_set writefds, exceptfds;
        FD_ZERO(&writefds);
        FD_ZERO(&exceptfds);
        FD_SET(sock, &writefds);
        FD_SET(sock, &exceptfds);
        timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select((int)sock + 1, nullptr, &writefds, &exceptfds, &tv) > 0 &&
            FD_ISSET(sock, &writefds)) {
            int so_error = 0;
#ifdef _WIN32
            int len = sizeof(so_error);
#else
            socklen_t len = sizeof(so_error);
#endif
            connected = getsockopt(sock, SOL_SOCKET, SO_ERROR,
                                   (char *)&so_error, &len) == 0 && so_error == 0;
        }
    }
    DISC_CLOSE(sock);
    return connected;
}

/**
 * Extract the SSDP NAME: header (device friendly name), line-based and
 * case-insensitive on the header name. Returns "" when absent.
 */
static std::string parseSsdpName(const std::string &response)
{
    size_t pos = 0;
    while (pos < response.size())
    {
        size_t eol = response.find_first_of("\r\n", pos);
        std::string line = response.substr(pos, (eol == std::string::npos) ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? response.size() : eol + 1;
        if (pos < response.size() && response[eol] == '\r' && response[pos] == '\n')
            pos++;

        if (line.size() < 5)
            continue;
        /* header name match "NAME:" case-insensitive */
        bool match = true;
        static const char *kName = "NAME:";
        for (int i = 0; i < 5; i++)
        {
            char c = line[(size_t)i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (c != kName[i]) { match = false; break; }
        }
        if (!match)
            continue;
        std::string val = line.substr(5);
        size_t a = val.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return std::string();
        size_t b = val.find_last_not_of(" \t\r\n");
        return val.substr(a, b - a + 1);
    }
    return std::string();
}

/**
 * Parse SSDP response to extract IP and port
 */
bool parseSsdpResponse(const std::string &response, std::string &ip, int &port,
                       std::string *name = nullptr)
{
    if (name)
        *name = parseSsdpName(response);

    size_t loc_pos = response.find("LOCATION: ");
    if (loc_pos == std::string::npos)
        loc_pos = response.find("Location: ");
    if (loc_pos == std::string::npos)
        loc_pos = response.find("Location");
    if (loc_pos == std::string::npos)
        return false;

    size_t line_end = response.find_first_of("\r\n", loc_pos);
    if (line_end == std::string::npos) {
        return false;
    }

    switch (response[line_end]) {
        case '\r':
            if (line_end + 1 < response.length() && response[line_end + 1] == '\n') {
                break; 
            }
            return false; 

        case '\n':
            break;

        default:
            return false;
    }

    /* Value starts after the header name's colon — don't hardcode "LOCATION: " length */
    size_t colon = response.find(':', loc_pos);
    if (colon == std::string::npos || colon >= line_end)
        return false;
    std::string url = response.substr(colon + 1, line_end - colon - 1);

    url.erase(0, url.find_first_not_of(" \t\r\n"));
    url.erase(url.find_last_not_of(" \t\r\n") + 1);
    size_t protocol_pos = url.find("://");
    if (protocol_pos == std::string::npos)
        return false;

    size_t host_start = protocol_pos + 3;
    size_t host_end = url.find_first_of(":/", host_start);

    if (host_end == std::string::npos)
    {
        ip = url.substr(host_start);
        port = 8081;
    }
    else
    {
        ip = url.substr(host_start, host_end - host_start);

        if (url[host_end] == ':')
        {
            size_t port_end = url.find("/", host_end);
            std::string port_str = url.substr(host_end + 1,
                                              (port_end == std::string::npos) ? std::string::npos : port_end - host_end - 1);
            try
            {
                port = std::stoi(port_str);
            }
            catch (...)
            {
                port = 8081;
            }
        }
        else
        {
            port = 8081;
        }
    }

    return true;
}

/**
 * Discover receivers on the network
 */
std::vector<DiscoveredDevice> discoverReceivers(int timeout_seconds,
                                                const DiscoveryOptions &options)
{
    std::vector<DiscoveredDevice> discovered;
    std::cout << "🔍 Scanning for receivers on 239.255.255.250:1900..." << std::endl;
    if (!initSockets()) return discovered;

    struct DiscoverySocket {
        disc_sock_t fd;
        sockaddr_in directed;
        bool has_directed = false;
    };
    std::vector<DiscoverySocket> sockets;
    const auto closeSockets = [&] {
        for (const DiscoverySocket &entry : sockets) DISC_CLOSE(entry.fd);
        sockets.clear();
        cleanupSockets();
    };
    const auto cancelled = [&] {
        return options.cancelled && options.cancelled->load();
    };

    std::vector<std::string> local_ips = listLocalIPv4();
    if (local_ips.empty()) local_ips.push_back("");
    const std::vector<LocalIface> ifaces = listLocalIPv4Ifaces();
    for (const std::string &lip : local_ips) {
        disc_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == DISC_INVALID) continue;
#ifndef _WIN32
        if (s >= FD_SETSIZE) { DISC_CLOSE(s); continue; }
#endif
        int reuse = 1, bc = 1, buf_size = SOCKET_BUFFER_SIZE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&buf_size, sizeof(buf_size));
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&bc, sizeof(bc));
#ifdef _WIN32
        u_long one = 1;
        if (ioctlsocket(s, FIONBIO, &one) != 0) { DISC_CLOSE(s); continue; }
#else
        const int flags = fcntl(s, F_GETFL, 0);
        if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
            DISC_CLOSE(s);
            continue;
        }
#endif
        sockaddr_in bind_addr = {};
        bind_addr.sin_family = AF_INET;
        if (!lip.empty() && inet_pton(AF_INET, lip.c_str(), &bind_addr.sin_addr) != 1) {
            DISC_CLOSE(s);
            continue;
        }
        if (bind(s, (sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            DISC_CLOSE(s);
            continue;
        }
        DiscoverySocket entry = {};
        entry.fd = s;
        if (!lip.empty()) {
            ip_mreq mreq = {};
            mreq.imr_multiaddr.s_addr = inet_addr(SSDP_MULTICAST_GROUP);
            inet_pton(AF_INET, lip.c_str(), &mreq.imr_interface);
            setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq));
            in_addr ifaddr = {};
            inet_pton(AF_INET, lip.c_str(), &ifaddr);
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (char *)&ifaddr, sizeof(ifaddr));
            for (const LocalIface &iface : ifaces) {
                if (iface.ip != lip || iface.prefix <= 0 || iface.prefix >= 32) continue;
                in_addr ip_addr = {};
                if (inet_pton(AF_INET, lip.c_str(), &ip_addr) != 1) break;
                const uint32_t ip = ntohl(ip_addr.s_addr);
                const uint32_t mask = ~((1u << (32 - iface.prefix)) - 1);
                entry.directed.sin_family = AF_INET;
                entry.directed.sin_port = htons(SSDP_MULTICAST_PORT);
                entry.directed.sin_addr.s_addr = htonl((ip & mask) | ~mask);
                entry.has_directed = true;
                break;
            }
        }
        sockets.push_back(entry);
    }
    if (sockets.empty()) {
        std::cerr << "X. Failed to create discovery sockets" << std::endl;
        cleanupSockets();
        return discovered;
    }

    const std::string msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: urn:screen-share:receiver\r\n"
        "USER-AGENT: ScreenShare/1.0\r\n\r\n";
    sockaddr_in multicast = {}, broadcast = {};
    multicast.sin_family = broadcast.sin_family = AF_INET;
    multicast.sin_port = broadcast.sin_port = htons(SSDP_MULTICAST_PORT);
    inet_pton(AF_INET, SSDP_MULTICAST_GROUP, &multicast.sin_addr);
    broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(std::max(1, timeout_seconds));
    int round = 0;
    std::cout << "📡 Listening on " << sockets.size()
              << " NIC(s) for up to " << timeout_seconds << " seconds..." << std::endl;
    while (!cancelled() && discovered.size() < 30) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        if (round < 3 && now >= start + std::chrono::milliseconds(round * 100)) {
            for (const DiscoverySocket &entry : sockets) {
                sendto(entry.fd, msearch.data(), (int)msearch.size(), 0,
                       (sockaddr *)&multicast, sizeof(multicast));
                sendto(entry.fd, msearch.data(), (int)msearch.size(), 0,
                       (sockaddr *)&broadcast, sizeof(broadcast));
                if (entry.has_directed)
                    sendto(entry.fd, msearch.data(), (int)msearch.size(), 0,
                           (const sockaddr *)&entry.directed, sizeof(entry.directed));
            }
            round++;
        }

        int wait_ms = 50;
        if (round < 3) {
            const auto next = start + std::chrono::milliseconds(round * 100);
            wait_ms = (int)std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
                next - std::chrono::steady_clock::now()).count());
        }
        fd_set ready;
        FD_ZERO(&ready);
        disc_sock_t maxfd = 0;
        for (const DiscoverySocket &entry : sockets) {
            FD_SET(entry.fd, &ready);
            if (entry.fd > maxfd) maxfd = entry.fd;
        }
        timeval tv = {wait_ms / 1000, (wait_ms % 1000) * 1000};
        const int selected = select((int)maxfd + 1, &ready, nullptr, nullptr, &tv);
        if (selected < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            break;
        }
        if (selected == 0) continue;

        for (const DiscoverySocket &entry : sockets) {
            if (!FD_ISSET(entry.fd, &ready)) continue;
            for (;;) {
                char buffer[8192];
                sockaddr_in sender_addr = {};
                socklen_t sender_len = sizeof(sender_addr);
                const int bytes = (int)recvfrom(entry.fd, buffer, sizeof(buffer) - 1, 0,
                                                (sockaddr *)&sender_addr, &sender_len);
                if (bytes < 0) {
#ifdef _WIN32
                    const int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK || err == WSAEINTR) break;
#else
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
#endif
                    break;
                }
                if (bytes == 0) continue;
                std::string response(buffer, (size_t)bytes);
                if (response.find("urn:screen-share:receiver") == std::string::npos &&
                    response.find("screen-share") == std::string::npos) continue;
                std::string loc_ip, name;
                int port = 8081;
                if (!parseSsdpResponse(response, loc_ip, port, &name) ||
                    port < 1 || port > 65535) continue;
                char src[INET_ADDRSTRLEN] = {};
                if (!inet_ntop(AF_INET, &sender_addr.sin_addr, src, sizeof(src))) continue;
                const std::string ip = src;
                const bool duplicate = std::any_of(discovered.begin(), discovered.end(),
                    [&](const DiscoveredDevice &d) {
                        return d.ip_address == ip && d.tcp_port == port;
                    });
                if (duplicate) continue;
                discovered.emplace_back(ip, port);
                discovered.back().name = name;
                std::cout << "✓ Found receiver: " << ip << ":" << port << std::endl;
                if (options.onDevice) options.onDevice(discovered.back());
                if (discovered.size() >= 30) break;
            }
        }
    }

    closeSockets();
    std::sort(discovered.begin(), discovered.end(),
        [](const DiscoveredDevice &a, const DiscoveredDevice &b) {
            return a.ip_address < b.ip_address ||
                   (a.ip_address == b.ip_address && a.tcp_port < b.tcp_port);
        });
    std::cout << "Discovery complete: " << discovered.size()
              << " receiver(s) available" << std::endl;
    return discovered;
}

/**
 * Check if any receivers are available
 */
bool hasReceivers()
{
    return !discoverReceivers(2).empty();
}

/**
 * Format list of devices for display
 */
std::string listDevices(const std::vector<DiscoveredDevice> &devices)
{
    std::string list = "\n RECEIVERS FOUND:\n";
    if (devices.empty())
    {
        list += "  None found\n";
    }
    else
    {
        for (size_t i = 0; i < devices.size(); i++)
        {
            list += "  [" + std::to_string(i) + "] " + devices[i].toString();

            if (devices[i].ip_address == "127.0.0.1" ||
                devices[i].ip_address == getLocalIPAddress())
            {
                list += " (THIS MACHINE)";
            }
            list += "\n";
        }
    }
    return list;
}
