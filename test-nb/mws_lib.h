#ifndef WS_LIB_H
#define WS_LIB_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

    // WebSocket opcode values
#define WS_OPCODE_CONTINUATION 0x0
#define WS_OPCODE_TEXT         0x1
#define WS_OPCODE_BINARY       0x2
#define WS_OPCODE_CLOSE        0x8
#define WS_OPCODE_PING         0x9
#define WS_OPCODE_PONG         0xA

// WebSocket connection states
    typedef enum {
        WS_STATE_CONNECTING,
        WS_STATE_OPEN,
        WS_STATE_CLOSING,
        WS_STATE_CLOSED
    } ws_state;

    // WebSocket context structure
    struct ws_ctx {
        SOCKET socket;        // Socket handle for the WebSocket connection
        ws_state state;       // Current state of the WebSocket connection
        char* recv_buffer;    // Buffer to store received data
        size_t recv_buffer_size;  // Total size of the receive buffer
        size_t recv_buffer_len;   // Current length of data in the receive buffer
    };
    // WebSocket context
    typedef struct ws_ctx ws_ctx;

    // Initialize the WebSocket library
    int ws_init(void);

    // Check if server is available at TCP level
    int ws_check_server_available(const char* host, int port);

    // Create a new WebSocket context
    ws_ctx* ws_create_ctx(void);

    // Connect to a WebSocket server
    int ws_connect(ws_ctx* ctx, const char* uri);

    // Send data over the WebSocket
    int ws_send(ws_ctx* ctx, const char* data, size_t length, int opcode);

    // Receive data from the WebSocket (non-blocking)
    int ws_recv(ws_ctx* ctx, char* buffer, size_t buffer_size);

    // Close the WebSocket connection
    int ws_close(ws_ctx* ctx);

    // Destroy the WebSocket context
    void ws_destroy_ctx(ws_ctx* ctx);

    // Cleanup the WebSocket library
    void ws_cleanup(void);

    // Get the current state of the WebSocket connection
    ws_state ws_get_state(ws_ctx* ctx);

    // Process WebSocket events (should be called regularly)
    int ws_service(ws_ctx* ctx);

    void print_hex2(const uint8_t* data, size_t length);

#ifdef __cplusplus
}
#endif

#endif // WS_LIB_H
