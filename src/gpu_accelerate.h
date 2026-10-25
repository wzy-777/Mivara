/**
 * GPU_ACCELERATE.H - REMOTE GPU OFFLOAD API
 *
 * Include this in sender.cpp to use the receiver's GPU.
 * Include this in receiver.cpp to run the GPU service.
 */
#ifndef GPU_ACCELERATE_H
#define GPU_ACCELERATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ─── Socket type abstraction ─────────────────────────────────── */
#ifdef _WIN32
#include <winsock2.h>
    typedef SOCKET gpu_sock_t;
#define GPU_INVALID_SOCK INVALID_SOCKET
#define GPU_SOCK_VALID(s) ((s) != INVALID_SOCKET)
#else
typedef int gpu_sock_t;
#define GPU_INVALID_SOCK (-1)
#define GPU_SOCK_VALID(s) ((s) >= 0)
#endif

/* ─── Constants ───────────────────────────────────────────────── */
#define GPU_ACCEL_PORT 8082

    /** Return non-zero when the service should stop (NULL = never stop). */
    typedef int (*gpu_stop_fn)(void);

    /** Session authorization hook. The receiver installs one; every client
     *  must present the 32-byte token of an authorized 8081 session as the
     *  first bytes on the connection. Returns non-zero if the token is valid.
     *  Without a hook (old behaviour) the service is unauthenticated. */
    typedef int (*gpu_token_check_fn)(const uint8_t *token, uint32_t len);
    void gpu_service_set_token_check(gpu_token_check_fn fn);

    /** Real startup result of the 8082 listener (same fields as the ports
     *  service). state 0 = starting, 1 = listening, -1 = failed. */
    typedef struct gpu_startup_status
    {
        int      state;
        int      stage;
        int      error_code;
        uint16_t port;
    } gpu_startup_status;
    gpu_startup_status gpu_service_status(void);

    /* ── Sender-side (client) ─────────────────────────────────────── */

    /** Connect to the receiver's GPU service. Returns socket fd or -1.
     *  `token` is the 32-byte session token from the v3 handshake; the
     *  receiver's service refuses the connection without it. */
    gpu_sock_t gpu_remote_connect(const char *receiver_ip,
                                  const uint8_t *token);

    /** Compress a raw RGB frame on the receiver's GPU.
     *  Allocates *out_buf; caller must free(). Returns compressed size or -1. */
    int gpu_remote_compress(gpu_sock_t gpu_sock,
                            const uint8_t *pixels,
                            uint32_t width, uint32_t height,
                            uint8_t **out_buf);

    /** Fix BGR→RGB colour channels using the receiver's GPU.
     *  Modifies `pixels` in-place. Returns 0 on success. */
    int gpu_remote_colorfix(gpu_sock_t gpu_sock,
                            uint8_t *pixels,
                            uint32_t width, uint32_t height);

    /** Close GPU service connection. */
    void gpu_remote_disconnect(gpu_sock_t gpu_sock);

    /* ── Receiver-side (server) ───────────────────────────────────── */

    /** Handle one connected GPU-service client (blocking until disconnect).
     *  `should_stop` is polled at least once per second; NULL means never. */
    void gpu_service_handle_client(int client_sock, gpu_stop_fn should_stop);

    /** Start the GPU service listener – run in a dedicated thread (blocking).
     *  Returns within ~1 s of `should_stop` returning non-zero. */
    void gpu_service_run(gpu_stop_fn should_stop);

#ifdef __cplusplus
}
#endif

#endif /* GPU_ACCELERATE_H */