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
 * Prefers real LAN addresses over tunnel/VPN ranges (Clash TUN uses 198.18.x).
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
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET)
        return false;
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return false;
#endif

#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    int result = connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    bool connected = false;

#ifdef _WIN32
    if (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
#else
    if (result < 0 && errno == EINPROGRESS)
#endif
    {
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(sock + 1, NULL, &fdset, NULL, &tv) == 1)
        {
            int so_error;
            socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len);
            if (so_error == 0)
                connected = true;
        }
    }

#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif

    return connected;
}

/**
 * Parse SSDP response to extract IP and port
 */
bool parseSsdpResponse(const std::string &response, std::string &ip, int &port)
{
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
std::vector<DiscoveredDevice> discoverReceivers(int timeout_seconds)
{
    std::vector<DiscoveredDevice> discovered_receivers;

    std::cout << "🔍 Scanning for receivers on 239.255.255.250:1900..." << std::endl;

    if (!initSockets())
        return discovered_receivers;

    int reuse = 1;
    int sock_buf_size = SOCKET_BUFFER_SIZE;

    /* One socket per local NIC: binding to INADDR_ANY lets a VPN/TUN
       (e.g. Clash) steal the source address, and the receiver's reply
       then never comes back. */
    std::vector<std::string> local_ips = listLocalIPv4();
    if (local_ips.empty())
        local_ips.push_back(""); /* fall back to INADDR_ANY */

    std::vector<disc_sock_t> socks;
    for (const std::string &lip : local_ips)
    {
        disc_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == DISC_INVALID)
            continue;

        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&sock_buf_size, sizeof(sock_buf_size));
        int bc = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&bc, sizeof(bc));

        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = 0;
        if (lip.empty())
            bind_addr.sin_addr.s_addr = INADDR_ANY;
        else
            inet_pton(AF_INET, lip.c_str(), &bind_addr.sin_addr);

        if (bind(s, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
        {
            DISC_CLOSE(s);
            continue;
        }

        /* Scope multicast to this NIC as well */
        if (!lip.empty())
        {
            struct ip_mreq mreq;
            mreq.imr_multiaddr.s_addr = inet_addr(SSDP_MULTICAST_GROUP);
            inet_pton(AF_INET, lip.c_str(), &mreq.imr_interface);
            setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq));
            struct in_addr ifaddr;
            inet_pton(AF_INET, lip.c_str(), &ifaddr);
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (char *)&ifaddr, sizeof(ifaddr));
        }

#ifdef _WIN32
        /* Windows SO_RCVTIMEO takes DWORD milliseconds — a struct timeval here
           would be misread as a 5ms timeout and discovery would give up at once */
        DWORD tv = (DWORD)timeout_seconds * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));
#else
        struct timeval tv;
        tv.tv_sec = timeout_seconds;
        tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));
#endif

        socks.push_back(s);
    }

    if (socks.empty())
    {
        std::cerr << "X. Failed to create discovery sockets" << std::endl;
        cleanupSockets();
        return discovered_receivers;
    }

    std::string msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: urn:screen-share:receiver\r\n"
        "USER-AGENT: ScreenShare/1.0\r\n"
        "\r\n";

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SSDP_MULTICAST_PORT);
    inet_pton(AF_INET, SSDP_MULTICAST_GROUP, &dest_addr.sin_addr);

    /* Broadcast fallback: many enterprise networks drop multicast between
       hosts but still pass subnet broadcast. Same M-SEARCH, same port. */
    struct sockaddr_in bcast_addr;
    memset(&bcast_addr, 0, sizeof(bcast_addr));
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_port = htons(SSDP_MULTICAST_PORT);
    bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    std::cout << "📡 Sending SSDP M-SEARCH requests (multicast + broadcast, "
              << socks.size() << " NIC(s))..." << std::endl;
    for (int i = 0; i < 3; i++)
    {
        for (disc_sock_t s : socks)
        {
            sendto(s, msearch.c_str(), (int)msearch.length(), 0,
                   (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            sendto(s, msearch.c_str(), (int)msearch.length(), 0,
                   (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "📡 Waiting for responses (" << timeout_seconds << " seconds)..." << std::endl;

    char buffer[8192];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);

    int response_count = 0;
    while (response_count < 30)
    {
        bool got_any = false;
        for (disc_sock_t s : socks)
        {
            sender_len = sizeof(sender_addr);
            int bytes = (int)recvfrom(s, buffer, (int)sizeof(buffer) - 1, 0,
                                      (struct sockaddr *)&sender_addr, &sender_len);

            if (bytes < 0)
            {
                continue;
            }
            got_any = true;

            buffer[bytes] = '\0';
            std::string response(buffer);

            if (response.find("urn:screen-share:receiver") != std::string::npos ||
                response.find("screen-share") != std::string::npos)
            {
                std::string loc_ip;
                int port = 8081;
                parseSsdpResponse(response, loc_ip, port); /* best-effort: at least for the port */

                /* The datagram source address is by definition reachable —
                   prefer it over LOCATION, which multi-homed receivers may
                   fill with a wrong NIC. */
                std::string src_ip = inet_ntoa(sender_addr.sin_addr);
                std::string ip;
                if (testTcpConnection(src_ip, port, 500))
                {
                    ip = src_ip;
                }
                else if (!loc_ip.empty() && loc_ip != src_ip && testTcpConnection(loc_ip, port, 500))
                {
                    ip = loc_ip;
                    std::cout << "   (using LOCATION address " << loc_ip << ")" << std::endl;
                }

                if (!ip.empty())
                {
                    std::cout << "✓ Found receiver: " << ip << ":" << port << std::endl;
                    discovered_receivers.emplace_back(ip, port);
                }
                else
                {
                    std::cout << "   X. Receiver at " << src_ip << ":" << port
                              << " not reachable (LOCATION: " << loc_ip << ")" << std::endl;
                }
                response_count++;
            }
        }
        /* Every socket timed out with nothing left to read */
        if (!got_any)
            break;
    }

    for (disc_sock_t s : socks)
        DISC_CLOSE(s);

    cleanupSockets();

    /* Dedupe: sort first — std::unique only collapses adjacent duplicates */
    std::sort(discovered_receivers.begin(), discovered_receivers.end(),
              [](const DiscoveredDevice &a, const DiscoveredDevice &b)
              {
                  return a.ip_address < b.ip_address ||
                         (a.ip_address == b.ip_address && a.tcp_port < b.tcp_port);
              });
    discovered_receivers.erase(
        std::unique(discovered_receivers.begin(), discovered_receivers.end(),
                    [](const DiscoveredDevice &a, const DiscoveredDevice &b)
                    {
                        return a.ip_address == b.ip_address && a.tcp_port == b.tcp_port;
                    }),
        discovered_receivers.end());

    std::cout << "Discovery complete: " << discovered_receivers.size() << " receiver(s) available" << std::endl;
    return discovered_receivers;
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