/**
 * GPU_ACCELERATE.C  –  REMOTE COMPUTE OFFLOAD ENGINE  (v2)
 *
 * Real measured timing (not hardcoded), real CPU work on receiver.
 */

#include "gpu_accelerate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#endif

/* Wire constants */
#define GPU_MAGIC        0x47505541u
#define GPU_VERSION      1
#define GPU_OP_PING      0xFF
#define GPU_OP_COMPRESS  0x01
#define GPU_OP_COLORCONV 0x03

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  operation;
    uint16_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t data_size;
} GpuRequest;

typedef struct {
    uint32_t magic;
    uint8_t  status;
    uint8_t  operation;
    uint16_t flags;
    uint32_t result_size;
    float    ms_elapsed;
} GpuResponse;
#pragma pack(pop)


/* ─── Real monotonic timer (ms) ─────────────────────────────── */
static double now_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

/* ─── Bounded, stop-aware I/O ──────────────────────────────────── */
static int recv_all_stop(int sock, void *buf, size_t size, gpu_stop_fn stop);
static int send_all_stop(int sock, const void *buf, size_t size, gpu_stop_fn stop);

static int send_all(int sock, const void *buf, size_t size)
{
    return send_all_stop(sock, buf, size, NULL);
}

static int recv_all(int sock, void *buf, size_t size)
{
    return recv_all_stop(sock, buf, size, NULL);
}

#ifdef _WIN32
#define GPU_FD_SET(s, set) FD_SET((SOCKET)(s), (set))
#else
#define GPU_FD_SET(s, set) FD_SET((s), (set))
#endif

static int gpu_wait(int sock, int for_write, gpu_stop_fn stop, double deadline)
{
    for (;;) {
        fd_set fds;
        struct timeval tv;
        int r;
        if ((stop && stop()) || now_ms() >= deadline) return -1;
        const double remaining = deadline - now_ms();
        FD_ZERO(&fds);
#ifndef _WIN32
        if (sock >= FD_SETSIZE) return -1;
#endif
        GPU_FD_SET(sock, &fds);
        tv.tv_sec = remaining >= 1000.0 ? 1 : 0;
        tv.tv_usec = remaining >= 1000.0 ? 0 : (long)(remaining * 1000.0);
#ifdef _WIN32
        r = select(0, for_write ? NULL : &fds, for_write ? &fds : NULL,
                   NULL, &tv);
#else
        r = select(sock + 1, for_write ? NULL : &fds,
                   for_write ? &fds : NULL, NULL, &tv);
#endif
        if (r > 0) return 0;
        if (r < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return -1;
        }
    }
}

/* Accepted sockets are put in non-blocking mode: there is no way to bound a
 * blocking send() with select() alone (select only promises *some* space),
 * so a stalled peer could otherwise wedge the service past the stop flag. */
static void gpu_set_nonblocking(int sock)
{
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket((SOCKET)sock, FIONBIO, &one);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int recv_all_stop(int sock, void *buf, size_t size, gpu_stop_fn stop)
{
    char *p = (char *)buf;
    size_t got = 0;
    const double deadline = now_ms() + 30000.0;
    while (got < size) {
        int n;
        if (now_ms() >= deadline || gpu_wait(sock, 0, stop, deadline) < 0) return -1;
        n = (int)recv(sock, p + got, (int)(size - got), 0);
        if (n <= 0) {
#ifdef _WIN32
            if (n < 0 && (WSAGetLastError() == WSAEINTR ||
                          WSAGetLastError() == WSAEWOULDBLOCK)) continue;
#else
            if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                          errno == EWOULDBLOCK)) continue;
#endif
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

static int send_all_stop(int sock, const void *buf, size_t size, gpu_stop_fn stop)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    const double deadline = now_ms() + 30000.0;
    while (sent < size) {
        int n;
        if (now_ms() >= deadline || gpu_wait(sock, 1, stop, deadline) < 0) return -1;
        n = (int)send(sock, p + sent, (int)(size - sent), 0);
        if (n <= 0) {
#ifdef _WIN32
            if (n < 0 && (WSAGetLastError() == WSAEINTR ||
                          WSAGetLastError() == WSAEWOULDBLOCK)) continue;
#else
            if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                          errno == EWOULDBLOCK)) continue;
#endif
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int gpu_frame_bytes(uint32_t width, uint32_t height, uint32_t *out)
{
    if (width == 0 || height == 0 || width > 7680 || height > 4320) return 0;
    const uint64_t bytes = (uint64_t)width * (uint64_t)height * 3u;
    if (bytes > 7680u * 4320u * 3u) return 0;
    *out = (uint32_t)bytes;
    return 1;
}

/* ─── BGR → RGB swap ─────────────────────────────────────────── */
static void fix_colour_channels(uint8_t *pixels, uint32_t width, uint32_t height)
{
    const size_t total = (size_t)width * (size_t)height;
    for (size_t i = 0; i < total; i++) {
        uint8_t tmp       = pixels[i * 3];
        pixels[i * 3]     = pixels[i * 3 + 2];
        pixels[i * 3 + 2] = tmp;
    }
}

/* ─── RLE compressor ─────────────────────────────────────────── */
static uint32_t rle_compress(const uint8_t *src, uint32_t src_len,
                              uint8_t *dst,       uint32_t dst_cap)
{
    uint32_t in = 0, out = 0;
    while (in + 2 < src_len) {
        if (out + 4 > dst_cap) break;
        uint8_t r   = src[in];
        uint8_t g   = src[in + 1];
        uint8_t b   = src[in + 2];
        uint8_t run = 1;
        while (run < 255
               && in + (uint32_t)run * 3 + 2 < src_len
               && src[in + run*3]     == r
               && src[in + run*3 + 1] == g
               && src[in + run*3 + 2] == b)
        { run++; }
        dst[out++] = run;
        dst[out++] = r;
        dst[out++] = g;
        dst[out++] = b;
        in += (uint32_t)run * 3;
    }
    return out;
}

/* ═══════════════════════════════════════════════════════════════
 * SENDER-SIDE CLIENT
 * ═══════════════════════════════════════════════════════════════ */

static void gpu_close_sock(gpu_sock_t sock)
{
#ifdef _WIN32
    closesocket((SOCKET)sock);
#else
    close((int)sock);
#endif
}

gpu_sock_t gpu_remote_connect(const char *receiver_ip)
{
    gpu_sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (!GPU_SOCK_VALID(sock)) return GPU_INVALID_SOCK;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(GPU_ACCEL_PORT);
    if (!receiver_ip || inet_pton(AF_INET, receiver_ip, &addr.sin_addr) != 1) {
        gpu_close_sock(sock);
        return GPU_INVALID_SOCK;
    }
    gpu_set_nonblocking((int)sock);
    int connected = connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    if (!connected) {
        int pending = 0;
#ifdef _WIN32
        const int err = WSAGetLastError();
        pending = err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
        pending = errno == EINPROGRESS;
#endif
        const double deadline = now_ms() + 5000.0;
        while (pending && now_ms() < deadline) {
            if (gpu_wait((int)sock, 1, NULL, deadline) < 0) break;
            int so_error = 0;
#ifdef _WIN32
            int len = sizeof(so_error);
#else
            socklen_t len = sizeof(so_error);
#endif
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len) == 0 &&
                so_error == 0) {
                connected = 1;
            }
            break;
        }
    }
    if (!connected) {
        gpu_close_sock(sock);
        fprintf(stderr, "[GPU] Cannot connect to %s:%d\n", receiver_ip, GPU_ACCEL_PORT);
        return GPU_INVALID_SOCK;
    }

    GpuRequest ping;
    memset(&ping, 0, sizeof(ping));
    ping.magic = htonl(GPU_MAGIC);
    ping.version = GPU_VERSION;
    ping.operation = GPU_OP_PING;
    GpuResponse pong;
    memset(&pong, 0, sizeof(pong));
    if (send_all((int)sock, &ping, sizeof(ping)) < 0 ||
        recv_all((int)sock, &pong, sizeof(pong)) < 0 ||
        ntohl(pong.magic) != GPU_MAGIC || pong.status != 0 ||
        pong.operation != GPU_OP_PING) {
        gpu_close_sock(sock);
        return GPU_INVALID_SOCK;
    }
    printf("[GPU] Connected to receiver compute service at %s:%d\n",
           receiver_ip, GPU_ACCEL_PORT);
    return sock;
}

int gpu_remote_compress(gpu_sock_t gpu_sock,
                        const uint8_t *pixels,
                        uint32_t width, uint32_t height,
                        uint8_t **out_buf)
{
    uint32_t data_size = 0;
    if (!out_buf || !pixels || !gpu_frame_bytes(width, height, &data_size)) return -1;
    *out_buf = NULL;
    GpuRequest req;
    memset(&req, 0, sizeof(req));
    req.magic = htonl(GPU_MAGIC);
    req.version = GPU_VERSION;
    req.operation = GPU_OP_COMPRESS;
    req.width = htonl(width);
    req.height = htonl(height);
    req.data_size = htonl(data_size);
    if (send_all((int)gpu_sock, &req, sizeof(req)) < 0 ||
        send_all((int)gpu_sock, pixels, data_size) < 0) return -1;

    GpuResponse resp;
    memset(&resp, 0, sizeof(resp));
    if (recv_all((int)gpu_sock, &resp, sizeof(resp)) < 0 ||
        ntohl(resp.magic) != GPU_MAGIC || resp.status != 0 ||
        resp.operation != GPU_OP_COMPRESS) return -1;
    const uint32_t result_size = ntohl(resp.result_size);
    if (result_size == 0 || result_size > (uint32_t)INT32_MAX ||
        result_size > data_size * 2u + 4096u) return -1;
    *out_buf = (uint8_t *)malloc(result_size);
    if (!*out_buf) return -1;
    if (recv_all((int)gpu_sock, *out_buf, result_size) < 0) {
        free(*out_buf);
        *out_buf = NULL;
        return -1;
    }
    printf("[GPU] Compress: %u → %u bytes (%.1f%%) in %.2f ms [receiver CPU]\n",
           data_size, result_size, 100.0f * (float)result_size / (float)data_size,
           resp.ms_elapsed);
    return (int)result_size;
}

int gpu_remote_colorfix(gpu_sock_t gpu_sock,
                        uint8_t *pixels,
                        uint32_t width, uint32_t height)
{
    uint32_t data_size = 0;
    if (!pixels || !gpu_frame_bytes(width, height, &data_size)) return -1;
    GpuRequest req;
    memset(&req, 0, sizeof(req));
    req.magic = htonl(GPU_MAGIC);
    req.version = GPU_VERSION;
    req.operation = GPU_OP_COLORCONV;
    req.width = htonl(width);
    req.height = htonl(height);
    req.data_size = htonl(data_size);
    if (send_all((int)gpu_sock, &req, sizeof(req)) < 0 ||
        send_all((int)gpu_sock, pixels, data_size) < 0) return -1;
    GpuResponse resp;
    memset(&resp, 0, sizeof(resp));
    if (recv_all((int)gpu_sock, &resp, sizeof(resp)) < 0 ||
        ntohl(resp.magic) != GPU_MAGIC || resp.status != 0 ||
        resp.operation != GPU_OP_COLORCONV ||
        ntohl(resp.result_size) != data_size) return -1;
    if (recv_all((int)gpu_sock, pixels, data_size) < 0) return -1;
    printf("[GPU] Colorfix %ux%u in %.2f ms [receiver CPU]\n",
           width, height, resp.ms_elapsed);
    return 0;
}

void gpu_remote_disconnect(gpu_sock_t gpu_sock)
{
    if (GPU_SOCK_VALID(gpu_sock)) {
        gpu_close_sock(gpu_sock);
        printf("[GPU] Disconnected from receiver compute service\n");
    }
}

/* ═══════════════════════════════════════════════════════════════
 * RECEIVER-SIDE SERVER
 * ═══════════════════════════════════════════════════════════════ */

void gpu_service_handle_client(int client_sock, gpu_stop_fn should_stop)
{
    printf("[GPU-SVC] Client connected\n");
    for (;;) {
        GpuRequest req;
        if (should_stop && should_stop()) break;
        if (recv_all_stop(client_sock, &req, sizeof(req), should_stop) < 0) break;
        if (ntohl(req.magic) != GPU_MAGIC || req.version != GPU_VERSION) {
            fprintf(stderr, "[GPU-SVC] Bad magic or version\n"); break;
        }

        uint8_t  op      = req.operation;
        uint32_t width   = ntohl(req.width);
        uint32_t height  = ntohl(req.height);
        uint32_t data_sz = ntohl(req.data_size);

        if (op == GPU_OP_PING) {
            GpuResponse resp;
            memset(&resp, 0, sizeof(resp));
            resp.magic     = htonl(GPU_MAGIC);
            resp.status    = 0;
            resp.operation = GPU_OP_PING;
            if (send_all_stop(client_sock, &resp, sizeof(resp), should_stop) < 0) break;
            continue;
        }

        uint32_t expected = 0;
        if ((op != GPU_OP_COMPRESS && op != GPU_OP_COLORCONV) ||
            !gpu_frame_bytes(width, height, &expected) || data_sz != expected) {
            fprintf(stderr, "[GPU-SVC] Invalid operation or frame size: op=%u %ux%u bytes=%u\n",
                    op, width, height, data_sz);
            break;
        }

        uint8_t *buf = (uint8_t *)malloc(data_sz);
        if (!buf) break;

        double t0 = now_ms();
        if (recv_all_stop(client_sock, buf, data_sz, should_stop) < 0) { free(buf); break; }

        uint8_t  *result    = NULL;
        uint32_t  result_sz = 0;
        uint8_t   status    = 0;

        if (op == GPU_OP_COMPRESS) {
            uint32_t max_out = data_sz * 2 + 4096;
            result = (uint8_t *)malloc(max_out);
            if (result) {
                result_sz = rle_compress(buf, data_sz, result, max_out);
                if (result_sz == 0) {
                    /* incompressible – send raw */
                    free(result);
                    result    = buf;
                    result_sz = data_sz;
                    buf       = NULL;
                } else {
                    free(buf); buf = NULL;
                }
            } else {
                status = 1; free(buf); buf = NULL;
            }
        } else if (op == GPU_OP_COLORCONV) {
            fix_colour_channels(buf, width, height);
            result    = buf;
            result_sz = data_sz;
            buf       = NULL;
        } else {
            fprintf(stderr, "[GPU-SVC] Unknown op 0x%02X\n", op);
            status = 2; free(buf); buf = NULL;
        }

        double t1 = now_ms();
        float  ms = (float)(t1 - t0);

        if (op == GPU_OP_COMPRESS && result_sz > 0) {
            printf("[GPU-SVC] Compressed %u → %u bytes (%.1f%%) in %.2f ms\n",
                   data_sz, result_sz,
                   100.0f * (float)result_sz / (float)data_sz, ms);
        } else if (op == GPU_OP_COLORCONV) {
            printf("[GPU-SVC] Colorfix %ux%u in %.2f ms\n", width, height, ms);
        }

        GpuResponse resp;
        memset(&resp, 0, sizeof(resp));
        resp.magic       = htonl(GPU_MAGIC);
        resp.status      = status;
        resp.operation   = op;
        resp.result_size = htonl(result_sz);
        resp.ms_elapsed  = ms;

        if (send_all_stop(client_sock, &resp, sizeof(resp), should_stop) < 0) { free(result); break; }
        if (result && result_sz > 0)
            if (send_all_stop(client_sock, result, result_sz, should_stop) < 0) { free(result); break; }
        free(result);
    }
    printf("[GPU-SVC] Client disconnected\n");
}

void gpu_service_run(gpu_stop_fn should_stop)
{
#ifdef _WIN32
    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) { fprintf(stderr, "[GPU-SVC] socket() failed\n"); return; }
#else
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { fprintf(stderr, "[GPU-SVC] socket() failed\n"); return; }
#endif
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(GPU_ACCEL_PORT);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[GPU-SVC] bind() failed on port %d\n", GPU_ACCEL_PORT);
#ifdef _WIN32
        closesocket(server);
#else
        close(server);
#endif
        return;
    }
    listen(server, 4);
    printf("[GPU-SVC] Compute service listening on TCP %d\n", GPU_ACCEL_PORT);
#ifndef _WIN32
    /* A peer that vanished mid-frame must not take the receiver with it. */
    signal(SIGPIPE, SIG_IGN);
#endif
    for (;;) {
        fd_set fds;
        struct timeval tv;
        int ready;

        if (should_stop && should_stop()) break;

        FD_ZERO(&fds);
        GPU_FD_SET(server, &fds);
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
#ifdef _WIN32
        ready = select(0, &fds, NULL, NULL, &tv);
#else
        ready = select((int)server + 1, &fds, NULL, NULL, &tv);
#endif
        if (ready < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            fprintf(stderr, "[GPU-SVC] select() failed\n");
            break;
        }
        if (ready == 0) continue;   /* timeout: re-check the stop flag */

        struct sockaddr_in ca;
        memset(&ca, 0, sizeof(ca));
#ifdef _WIN32
        int calen = sizeof(ca);
        SOCKET client = accept(server, (struct sockaddr *)&ca, &calen);
        if (client == INVALID_SOCKET) {
            if (should_stop && should_stop()) break;
            continue;
        }
#else
        socklen_t calen = sizeof(ca);
        int client = accept(server, (struct sockaddr *)&ca, &calen);
        if (client < 0) {
            if (should_stop && should_stop()) break;
            continue;
        }
#endif
        printf("[GPU-SVC] Connection from %s\n", inet_ntoa(ca.sin_addr));
        gpu_set_nonblocking((int)client);
        gpu_service_handle_client((int)client, should_stop);
#ifdef _WIN32
        closesocket(client);
#else
        close(client);
#endif
    }
#ifdef _WIN32
    closesocket(server);
#else
    close(server);
#endif
}
