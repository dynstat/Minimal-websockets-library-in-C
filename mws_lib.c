// mws_lib.c - WebSocket client-side library implementation
// This version has been updated to improve RFC 6455 compliance for a client-only implementation.
// 
// Changes include:
//   • A helper function (apply_mask) to apply the masking using XOR so that all outbound frames are masked.
//   • A refactored ws_handle_ping() that uses ws_send() to send a proper, masked PONG frame echoing the ping payload.
//   • Improved server availability checking with select() monitoring both write and exception sets.
//   • A ws_check_connection() update that calls getsockopt() to check for socket errors.
//   • A definition for ntohll() so that 64-bit payload lengths are correctly converted from network byte order.

#include "mws_lib.h"
#include "Logger2.h"

// #define WIN32_LEAN_AND_MEAN
// #include <windows.h>
// #include <winsock2.h>
// #include <ws2tcpip.h>

// Link with Ws2_32.lib for Windows sockets.
#pragma comment(lib, "Ws2_32.lib")

// WebSocket-specific constants (GUID, header size, heartbeat intervals, etc.)
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_HEADER_SIZE 14

#define HEARTBEAT_INTERVAL 30 // seconds
#define HEARTBEAT_TIMEOUT 10  // seconds

#define PING_TIMEOUT_MS 30000  // 30 seconds
#define PONG_TIMEOUT_MS 10000  // 10 seconds

// If ntohll is not already defined, define it here.
// On Windows, _byteswap_uint64() is used; on other platforms, be64toh() is assumed.
#ifndef ntohll
#ifdef _WIN32
#include <stdlib.h>
#define ntohll(x) _byteswap_uint64(x)
#else
#include <endian.h>
#define ntohll(x) be64toh(x)
#endif
#endif

// Base64 encoding table used during handshake key generation.
static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

//------------------------------------------------------------------------------
// Helper function: apply_mask
//
// This function applies the 32-bit mask over the data using an XOR
// so that each outbound frame is masked as required by RFC 6455.
//
// For example, if mask is 0x3AF27BC4 and the data is {0x55, 0xA2, 0xFF},
// then each byte is XORed with the corresponding mask byte (cycling through 4 bytes).
//------------------------------------------------------------------------------
static void apply_mask(uint8_t* data, size_t length, uint32_t mask) {
    for (size_t i = 0; i < length; i++) {
        data[i] ^= ((uint8_t*)&mask)[i % 4];
    }
}

//------------------------------------------------------------------------------
// Function: base64_encode
//
// Encodes the input data into a Base64 string.
//------------------------------------------------------------------------------
static char* base64_encode(const unsigned char* input, int length) {
    int output_length = 4 * ((length + 2) / 3);
    char* encoded = (char*)malloc(output_length + 1);
    if (encoded == NULL) return NULL;

    int i, j;
    for (i = 0, j = 0; i < length;) {
        uint32_t octet_a = i < length ? input[i++] : 0;
        uint32_t octet_b = i < length ? input[i++] : 0;
        uint32_t octet_c = i < length ? input[i++] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded[j++] = base64_table[(triple >> 18) & 0x3F];
        encoded[j++] = base64_table[(triple >> 12) & 0x3F];
        encoded[j++] = base64_table[(triple >> 6) & 0x3F];
        encoded[j++] = base64_table[triple & 0x3F];
    }

    for (i = 0; i < (3 - length % 3) % 3; i++)
        encoded[output_length - 1 - i] = '=';

    encoded[output_length] = '\0';
    return encoded;
}

//------------------------------------------------------------------------------
// Function: ws_send_handshake
//
// Constructs and sends the HTTP handshake request for establishing a WebSocket
// connection. A random 16-byte key is generated, Base64 encoded, and included.
//------------------------------------------------------------------------------
static int ws_send_handshake(ws_ctx* ctx, const char* host, const char* path) {
    logToFile2("MWS: Sending WebSocket handshake...\n");
    char key[16];
    char encoded_key[25];
    char request[1024];
    int request_len;

    // Assume srand() is called once in main.
    for (int i = 0; i < 16; i++) {
        key[i] = rand() % 256;
    }
    char* base64_key = base64_encode((unsigned char*)key, 16);
    strncpy_s(encoded_key, sizeof(encoded_key), base64_key, 24);
    encoded_key[24] = '\0';
    free(base64_key);
    logToFile2("MWS: Random key generated and encoded.\n");

    // Construct handshake HTTP request ensuring it is fully CRLF terminated.
    request_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, encoded_key);
    logToFile2("MWS: Handshake request constructed.\n");

    int total_sent = 0;
    // Loop to guarantee all bytes are sent properly
    while (total_sent < request_len) {
        int sent = send(ctx->socket, request + total_sent, request_len - total_sent, 0);
        if (sent <= 0) {
            logToFile2("MWS: Failed to send the complete handshake request.\n");
            return -1;
        }
        total_sent += sent;
    }
    logToFile2("MWS: Handshake request sent successfully.\n");
    return 0;
}

//------------------------------------------------------------------------------
// Function: ws_parse_handshake_response
//
// Reads the HTTP response for the handshake and verifies that the status and
// headers indicate a successful upgrade.
//------------------------------------------------------------------------------
static int ws_parse_handshake_response(ws_ctx* ctx) {
    logToFile2("MWS: Parsing WebSocket handshake response...\n");
    char buffer[2048];
    int total_received = 0;
    int bytes_received = 0;
    
    // Read data one byte at a time until the header terminator "\r\n\r\n" is found 
    // (or until the buffer is nearly full)
    while (total_received < (int)sizeof(buffer) - 1) {
        bytes_received = recv(ctx->socket, buffer + total_received, 1, 0);
        if (bytes_received <= 0) {
            logToFile2("MWS: Failed to receive handshake response\n");
            return -1;
        }
        total_received += bytes_received;
        buffer[total_received] = '\0';
        
        // Check for end-of-headers (CRLF CRLF)
        if (strstr(buffer, "\r\n\r\n") != NULL) {
            break;
        }
    }
    
    logToFile2("MWS: Received handshake response.\n");
    
    // Validate the handshake response:
    if (strstr(buffer, "HTTP/1.1 101") == NULL) {
        logToFile2("MWS: Invalid handshake response: HTTP/1.1 101 not found.\n");
        return -1;
    }
    if (strstr(buffer, "Upgrade: websocket") == NULL) {
        logToFile2("MWS: Invalid handshake response: Upgrade: websocket not found.\n");
        return -1;
    }
    if (strstr(buffer, "Sec-WebSocket-Accept:") == NULL) {
        logToFile2("MWS: Invalid handshake response: Sec-WebSocket-Accept header not found.\n");
        return -1;
    }
    
    ctx->state = WS_STATE_OPEN;
    logToFile2("MWS: WebSocket connection established successfully.\n");
    return 0;
}

//------------------------------------------------------------------------------
// Function: ws_init
//
// Initializes Winsock. Must be called before any WebSocket operations.
//------------------------------------------------------------------------------
int ws_init(void) {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData);
}

//------------------------------------------------------------------------------
// Function: ws_create_ctx
//
// Creates and initializes a new WebSocket context.
//------------------------------------------------------------------------------
ws_ctx* ws_create_ctx(void) {
    logToFile2("MWS: Creating WebSocket context...\n");
    ws_ctx* ctx = (ws_ctx*)malloc(sizeof(ws_ctx));
    if (!ctx) {
        logToFile2("MWS: Failed to allocate memory for WebSocket context\n");
        return NULL;
    }
    
    memset(ctx, 0, sizeof(ws_ctx));
    ctx->socket = INVALID_SOCKET;
    ctx->state = WS_STATE_CLOSED;
    ctx->recv_buffer = NULL;
    ctx->recv_buffer_size = 0;
    ctx->recv_buffer_len = 0;
    ctx->ping_interval = 30;  // Default to 30 seconds
    ctx->last_ping_time = time(NULL);
    
    return ctx;
}

//------------------------------------------------------------------------------
// Static helper: try_connect_nonblocking
//
// Attempts a connection in non-blocking mode with a given timeout.
//------------------------------------------------------------------------------
static int try_connect_nonblocking(SOCKET sock, const struct addrinfo* addr, int timeout_sec) {
    unsigned long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        logToFile2("MWS: Failed to set non-blocking mode\n");
        return -1;
    }
    int result = connect(sock, addr->ai_addr, (int)addr->ai_addrlen);
    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            char errMsg[256];
            snprintf(errMsg, sizeof(errMsg), "Connect failed immediately with error: %d\n", error);
            logToFile2(errMsg);
            return -1;
        }
        fd_set write_fds, except_fds;
        FD_ZERO(&write_fds);
        FD_ZERO(&except_fds);
        FD_SET(sock, &write_fds);
        FD_SET(sock, &except_fds);
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        result = select(sock + 1, NULL, &write_fds, &except_fds, &tv);
        if (result == 0) {
            logToFile2("MWS: Connection attempt timed out\n");
            return -2;  // timeout indicator
        }
        if (result == SOCKET_ERROR) {
            logToFile2("MWS: Select failed\n");
            return -1;
        }
        if (FD_ISSET(sock, &except_fds)) {
            logToFile2("MWS: Connection failed (exception)\n");
            return -1;
        }
        int so_error;
        int optlen = sizeof(so_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &optlen) == 0) {
            if (so_error != 0) {
                char errMsg[256];
                snprintf(errMsg, sizeof(errMsg), "Connection failed with error: %d\n", so_error);
                logToFile2(errMsg);
                return -1;
            }
        }
    }
    mode = 0;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        logToFile2("MWS: Failed to set blocking mode\n");
        return -1;
    }
    return 0;
}

//------------------------------------------------------------------------------
// Function: ws_connect
//
// Parses the URI, resolves the server, connects using non-blocking mode,
// performs the handshake, and sets the WebSocket state to OPEN.
//------------------------------------------------------------------------------
int ws_connect(ws_ctx* ctx, const char* uri) {
    logToFile2("MWS: Attempting to connect to WebSocket server\n");
    char schema[10], host[256], path[256];
    int port;
    // Try parsing URI with an explicit port first.
    if (sscanf_s(uri, "%9[^:]://%255[^:/]:%d%255s", schema, (unsigned)sizeof(schema),
                 host, (unsigned)sizeof(host), &port, path, (unsigned)sizeof(path)) < 3) {
        // If no explicit port is provided, try without the port and assign default values.
        if (sscanf_s(uri, "%9[^:]://%255[^/]%255s", schema, (unsigned)sizeof(schema),
                     host, (unsigned)sizeof(host), path, (unsigned)sizeof(path)) < 3) {
            logToFile2("MWS: Failed to parse URI\n");
            return -1;
        }
        port = strcmp(schema, "wss") == 0 ? 443 : 80;
    }
    
    // If path is empty, default to '/'
    if (strlen(path) == 0) {
        strcpy_s(path, sizeof(path), "/"); // Default path to '/'
    }
    
    // Log the parsed URI values for debugging.
    {
        char logMsg[512];
        snprintf(logMsg, sizeof(logMsg), "Parsed URI: schema=%s, host=%s, port=%d, path=%s\n",
                 schema, host, port, path);
        logToFile2((const char *)logMsg);
    }
    
    struct addrinfo hints, *addr_info;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    int gai_result = getaddrinfo(host, port_str, &hints, &addr_info);
    if (gai_result != 0) {
        char errMsg[256];
        snprintf(errMsg, sizeof(errMsg), "getaddrinfo failed: %s\n", gai_strerror(gai_result));
        logToFile2(errMsg);
        return -1;
    }
    
    struct addrinfo *ptr;
    int connect_result = -1;
    for (ptr = addr_info; ptr != NULL; ptr = ptr->ai_next) {
        ctx->socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (ctx->socket == INVALID_SOCKET) {
            continue;
        }
        connect_result = try_connect_nonblocking(ctx->socket, ptr, 2); // 2-second timeout
        if (connect_result == 0) {
            break;  // Successfully connected
        }
        closesocket(ctx->socket);
        ctx->socket = INVALID_SOCKET;
    }
    
    freeaddrinfo(addr_info);
    
    if (connect_result != 0) {
        return -1;
    }
    
    ctx->state = WS_STATE_CONNECTING;
    if (ws_send_handshake(ctx, host, path) != 0) {
        logToFile2("MWS: Failed to send WebSocket handshake\n");
        closesocket(ctx->socket);
        return -1;
    }
    if (ws_parse_handshake_response(ctx) != 0) {
        logToFile2("MWS: Failed to parse WebSocket handshake response\n");
        closesocket(ctx->socket);
        return -1;
    }
    logToFile2("MWS: WebSocket connection established successfully\n");

    // // Set socket to non-blocking mode
    // u_long mode = 1;
    // if (ioctlsocket(ctx->socket, FIONBIO, &mode) != 0) {
    //     logToFile2("MWS: Failed to set socket to non-blocking mode\n");
    //     closesocket(ctx->socket);
    //     return -1;
    // }

    return 0;
}

//------------------------------------------------------------------------------
// Function: generate_mask
//
// Generates a random 32-bit mask for use in constructing client frames.
//------------------------------------------------------------------------------
static uint32_t generate_mask() {
    return ((uint32_t)rand() << 24) | ((uint32_t)rand() << 16) | ((uint32_t)rand() << 8) | (uint32_t)rand();
}

//------------------------------------------------------------------------------
// Function: ws_send
//
// Constructs and sends a masked WebSocket frame.
//------------------------------------------------------------------------------
int ws_send(ws_ctx* ctx, const char* data, size_t length, int opcode) {
    char logBuffer[256];
    snprintf(logBuffer, sizeof(logBuffer), "Sending WebSocket frame: opcode=0x%X, length=%zu\n", opcode, length);
    logToFile2(logBuffer);

    if (ctx->state != WS_STATE_OPEN) {
        return -1;
    }

    uint8_t header[14];
    size_t header_size = 2;
    uint32_t mask = generate_mask();

    // Set FIN (0x80) and opcode
    header[0] = 0x80 | (opcode & 0x0F);

    // Set payload length and the mask bit (0x80)
    if (length <= 125) {
        header[1] = 0x80 | (uint8_t)length;
    }
    else if (length <= 65535) {
        header[1] = 0x80 | 126;
        header[2] = (length >> 8) & 0xFF;
        header[3] = length & 0xFF;
        header_size += 2;
    }
    else {
        header[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (length >> ((7 - i) * 8)) & 0xFF;
        }
        header_size += 8;
    }

    // Append the 4-byte mask key.
    memcpy(header + header_size, &mask, 4);
    header_size += 4;

    // Allocate memory for the entire frame (header + masked payload).
    size_t frame_size = header_size + length;
    uint8_t* frame = (uint8_t*)malloc(frame_size);
    if (!frame) return -1;

    memcpy(frame, header, header_size);
    for (size_t i = 0; i < length; i++) {
        frame[header_size + i] = data[i] ^ ((uint8_t*)&mask)[i % 4];
    }
    int result = send(ctx->socket, (char*)frame, frame_size, 0);
    free(frame);
    if (result != frame_size) {
        return -1;
    }
    logToFile2("MWS: WebSocket frame sent successfully\n");
    return 0;
}

//------------------------------------------------------------------------------
// Function: ws_consume_full_frame
//
// Helper function to read and discard an entire WebSocket frame based on its header.
// Assumes the first 2 bytes of the header have already been peeked.
// Returns 0 on success, -1 on error or close.
//------------------------------------------------------------------------------
static int ws_consume_full_frame(ws_ctx* ctx, uint8_t header[2]) {
    int bytes_read;
    uint8_t actual_header[2];

    // Consume the peeked header
    bytes_read = recv(ctx->socket, (char*)actual_header, 2, 0);
    if (bytes_read != 2) {
        logToFile2("MWS: Failed to consume peeked header.\n");
        return -1;
    }

    // Re-parse header info (could differ from peek if data arrived in between, though unlikely)
    bool masked = (actual_header[1] & 0x80) != 0;
    uint64_t payload_length = actual_header[1] & 0x7F;

    // Read extended payload length if necessary
    if (payload_length == 126) {
        uint16_t extended_length;
        bytes_read = recv(ctx->socket, (char*)&extended_length, 2, 0);
        if (bytes_read != 2) return -1;
        payload_length = ntohs(extended_length);
    } else if (payload_length == 127) {
        uint64_t extended_length;
        bytes_read = recv(ctx->socket, (char*)&extended_length, 8, 0);
        if (bytes_read != 8) return -1;
        payload_length = ntohll(extended_length);
    }

    // Read and discard mask key if present
    if (masked) {
        char mask_key[4];
        bytes_read = recv(ctx->socket, mask_key, 4, 0);
        if (bytes_read != 4) return -1;
    }

    // Read and discard payload
    if (payload_length > 0) {
        char discard_buffer[1024];
        uint64_t remaining_to_discard = payload_length;
        while (remaining_to_discard > 0) {
            size_t discard_now = (remaining_to_discard < sizeof(discard_buffer)) ? (size_t)remaining_to_discard : sizeof(discard_buffer);
            bytes_read = recv(ctx->socket, discard_buffer, discard_now, 0);
            if (bytes_read <= 0) {
                 logToFile2("MWS: Error or close while consuming frame payload.\n");
                 return -1; // Error during consumption
            }
            remaining_to_discard -= bytes_read;
        }
    }
    return 0; // Successfully consumed
}

//------------------------------------------------------------------------------
// Function: ws_handle_control_frame
//
// Sets socket to non-blocking, peeks for a frame. Restores blocking mode.
// If a control frame was peeked, consumes it using blocking reads and handles it.
// Returns:
//   1 if a control frame was handled successfully.
//   0 if no data, a non-control frame, or WSAEWOULDBLOCK during peek.
//  -1 on error or if a CLOSE frame was handled (connection is now closing/closed).
//------------------------------------------------------------------------------
static int ws_handle_control_frame(ws_ctx* ctx) {
     if (!ctx || ctx->socket == INVALID_SOCKET || ctx->state != WS_STATE_OPEN) {
        return 0; // Nothing to do if not open
    }

    uint8_t header[2];
    int peek_bytes = 0;
    int result_status = 0; // Default: 0 (no action/no control frame)
    unsigned long nonblock_mode = 1;
    unsigned long block_mode = 0;

    // --- Perform Non-Blocking Peek ---
    // Set socket to non-blocking
    if (ioctlsocket(ctx->socket, FIONBIO, &nonblock_mode) != 0) {
        logToFile2("MWS: Failed to set non-blocking mode for peek.\n");
        ws_close(ctx); return -1;
    }

    // Attempt the peek
    peek_bytes = recv(ctx->socket, (char*)header, 2, MSG_PEEK);
    // *** CAPTURE ERROR IMMEDIATELY IF RECV FAILED ***
    int error_code_from_peek = (peek_bytes == SOCKET_ERROR) ? WSAGetLastError() : 0;

    // Restore blocking mode immediately after peek attempt
    if (ioctlsocket(ctx->socket, FIONBIO, &block_mode) != 0) {
        // Log failure to restore, but the original peek error (if any) is more important
        logToFile2("MWS: Failed to restore blocking mode after peek! Connection likely unstable.\n");
        // Consider closing based on the original peek error below, or just log and continue
        // ws_close(ctx); return -1; // Or handle based on error_code_from_peek
    }

    // --- Process Peek Result ---
    if (peek_bytes == SOCKET_ERROR) {
        // Use the saved error code
        // int error = WSAGetLastError(); // OLD: Called too late
        int error = error_code_from_peek; // NEW: Use the saved error code
        if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
            // logToFile2("MWS: No data available during non-blocking peek.\n"); // Can be noisy
            return 0; // Expected case when no data is ready
        } else {
            // Real socket error during peek
            char errMsg[256]; snprintf(errMsg, sizeof(errMsg), "MWS: Socket error during non-blocking peek: %d\n", error); logToFile2(errMsg);
            ws_close(ctx); // Close on error
            return -1;
        }
    } else if (peek_bytes == 0) {
        // Connection closed gracefully by peer
        logToFile2("MWS: Connection closed by peer (detected during non-blocking peek).\n");
        ws_close(ctx);
        return -1;
    } else if (peek_bytes != 2) {
        // Should not happen with TCP
         logToFile2("MWS: Non-blocking peek returned unexpected byte count. Closing.\n");
         ws_close(ctx);
         return -1;
    }

    // --- Peek successful, check opcode ---
    int opcode = header[0] & 0x0F;
    uint64_t payload_len_indicator = header[1] & 0x7F;

    if (opcode == WS_OPCODE_PING || opcode == WS_OPCODE_PONG || opcode == WS_OPCODE_CLOSE) {

        logToFile2("MWS: Non-blocking peek detected control frame. Attempting blocking consumption.\n");

        // Validate control frame constraints from peeked header
        if (payload_len_indicator > 125) {
            logToFile2("MWS: Error - Peeked control frame with invalid payload length > 125. Closing.\n");
            ws_fail_connection(ctx, 1002, "Protocol error");
            // Try to consume the bad frame using blocking read now
            ws_consume_full_frame(ctx, header); // Best effort consume
            return -1;
        }

        // --- Consume the full control frame using standard BLOCKING reads ---
        // (Socket is already back in blocking mode)
        uint8_t frame_buffer[125] = {0}; // Max payload for control frame
        uint8_t actual_header[2];
        int bytes_read;
        bool masked = false;
        uint64_t payload_length = 0;
        uint32_t mask_key = 0;

        // 1. Consume header
        bytes_read = recv(ctx->socket, (char*)actual_header, 2, 0);
        if (bytes_read <= 0) { logToFile2("MWS: Error/close consuming header.\n"); ws_close(ctx); return -1; }
        // Re-validate opcode just in case
        if ((actual_header[0] & 0x0F) != opcode) { logToFile2("MWS: Opcode changed between peek and read! Aborting.\n"); return -1; }

        masked = (actual_header[1] & 0x80) != 0;
        payload_length = actual_header[1] & 0x7F; // Already validated <= 125

        // 2. Consume mask key (if present)
        if (masked) {
            logToFile2("MWS: Warning - Consuming masked control frame from server.\n");
            bytes_read = recv(ctx->socket, (char*)&mask_key, 4, 0);
            if (bytes_read <= 0) { logToFile2("MWS: Error/close consuming mask.\n"); ws_close(ctx); return -1; }
        }

        // 3. Consume payload
        if (payload_length > 0) {
             size_t total_payload_read = 0;
             while(total_payload_read < payload_length) {
                 bytes_read = recv(ctx->socket, (char*)frame_buffer + total_payload_read, (size_t)(payload_length - total_payload_read), 0);
                 if (bytes_read <= 0) { // Error or connection closed during payload read
                     logToFile2("MWS: Error/close while reading control payload.\n");
                     ws_close(ctx);
                     return -1;
                 }
                 total_payload_read += bytes_read;
             }
        }

        // --- Frame fully consumed, now handle it ---
        result_status = 1; // Assume handled successfully unless error occurs below

        // Unmask payload if necessary
        if (masked) {
            apply_mask(frame_buffer, (size_t)payload_length, mask_key);
        }

        // Handle the specific control frame
        switch(opcode) {
            case WS_OPCODE_PING:
                logToFile2("MWS: Handled PING frame. Sending PONG.\n");
                if (ws_send(ctx, (char*)frame_buffer, (size_t)payload_length, WS_OPCODE_PONG) != 0) {
                     logToFile2("MWS: Failed to send PONG response.\n");
                     result_status = -1; // Mark as error
                }
                // result_status remains 1 if ws_send succeeded
                break;

            case WS_OPCODE_PONG:
                logToFile2("MWS: Handled PONG frame.\n");
                // result_status remains 1
                break;

            case WS_OPCODE_CLOSE:
                logToFile2("MWS: Handled CLOSE frame from server.\n");
                uint16_t received_code = 1005; char reason_buffer[124] = {0};
                if (payload_length >= 2) { memcpy(&received_code, frame_buffer, 2); received_code = ntohs(received_code); }
                if (payload_length > 2) { size_t reason_len = min(payload_length - 2, sizeof(reason_buffer) - 1); memcpy(reason_buffer, frame_buffer + 2, reason_len); }
                char logMsg[200]; snprintf(logMsg, sizeof(logMsg), "MWS: Server close code: %d, Reason: %s\n", received_code, reason_buffer); logToFile2(logMsg);
                // ws_close will be called outside the switch based on result_status
                result_status = -1; // Indicate connection should close
                break;
        }

        // If handling failed or it was CLOSE, ensure connection is closed
        if (result_status == -1 && ctx->state == WS_STATE_OPEN) {
            ws_close(ctx);
        }
        return result_status; // Return 1 (handled ok) or -1 (error/closed)

    } else {
        // Peek showed a non-control frame, do nothing.
        logToFile2("MWS: Peeked non-control frame. Leaving for ws_recv.\n");
        return 0;
    }
}

//------------------------------------------------------------------------------
// Function: ws_service
//
// Regularly called to service the WebSocket connection. Checks for and handles
// incoming control frames (PING, PONG, CLOSE). Sends periodic PING frames.
// Returns 0 on success, -1 on error or if connection closed.
//------------------------------------------------------------------------------
int ws_service(ws_ctx* ctx) {
    logToFile2("MWS: Servicing WebSocket connection...\n");

    if (!ctx || ctx->socket == INVALID_SOCKET) {
        logToFile2("MWS: Invalid context or socket in ws_service\n");
        return -1;
    }
    if (ctx->state != WS_STATE_OPEN) {
         logToFile2("MWS: ws_service called but state is not OPEN.\n");
         // Return 0 if CLOSING, -1 if already CLOSED or other invalid state?
         return (ctx->state == WS_STATE_CLOSING) ? 0 : -1;
    }

    // Check for and handle any incoming control frame
    int handle_result = ws_handle_control_frame(ctx);
    if (handle_result == -1) {
        logToFile2("MWS: ws_handle_control_frame indicated error or closure.\n");
        return -1; // Error or connection closed during control frame handling
    }
    // If handle_result was 1, a control frame was handled successfully.
    // If handle_result was 0, no control frame was pending.

    // Send periodic PING frames if enabled and interval elapsed
    if (ctx->ping_interval > 0) {
        time_t current_time = time(NULL);
        if (current_time - ctx->last_ping_time >= ctx->ping_interval) {
            logToFile2("MWS: Sending periodic PING frame.\n");
            // Use ws_send to send a masked PING frame (payload optional, empty here)
            if (ws_send(ctx, NULL, 0, WS_OPCODE_PING) != 0) {
                logToFile2("MWS: Failed to send PING frame.\n");
                ws_close(ctx); // Close if we can't send PING
                return -1;
            }
            ctx->last_ping_time = current_time;
            logToFile2("MWS: PING frame sent successfully.\n");
        }
    }
    
    return 0; // Service check completed without closing the connection
}

//------------------------------------------------------------------------------
// Function: ws_close
//
// Initiates the closing handshake by sending a masked CLOSE frame (status code 1000)
// and then performs an orderly shutdown.
//------------------------------------------------------------------------------
int ws_close(ws_ctx* ctx) {
    logToFile2("MWS: Initiating WebSocket closing handshake...\n");

    if (!ctx || ctx->socket == INVALID_SOCKET) {
        logToFile2("MWS: Invalid context or socket in ws_close\n");
        return -1;
    }

    // Only send close frame if connection was OPEN
    if (ctx->state == WS_STATE_OPEN) {
        ctx->state = WS_STATE_CLOSING; // Change state first
        logToFile2("MWS: State changed to CLOSING.\n");

        uint8_t close_frame[6];  // 2 bytes header + 4 bytes mask key (payload length is 2)
        close_frame[0] = 0x88; // FIN + CLOSE opcode
        close_frame[1] = 0x82; // Set masked bit (0x80) and payload length 2
        uint32_t mask = generate_mask();
        memcpy(close_frame + 2, &mask, 4);

        uint16_t status_code = htons(1000); // Normal closure
        uint8_t payload[2];
        memcpy(payload, &status_code, 2);

        // Apply masking to the payload
        apply_mask(payload, 2, mask);

        // Send the close frame (header + payload)
        if (send(ctx->socket, (char*)close_frame, 6, 0) != 6 ||
            send(ctx->socket, (char*)payload, 2, 0) != 2) {
            logToFile2("MWS: Failed to send close frame, forcing close.\n");
            // Proceed to force_close even if send fails
        } else {
            logToFile2("MWS: Client CLOSE frame sent.\n");
             // Optional: Wait briefly for server's CLOSE frame (best effort)
             // This part makes the close slightly less abrupt but isn't strictly
             // required if immediate cleanup is preferred.
             // unsigned long nb_mode = 1, b_mode = 0;
             // struct timeval tv = {0, 100000}; // 100ms timeout
             // fd_set readfds;
             // FD_ZERO(&readfds); FD_SET(ctx->socket, &readfds);
             // ioctlsocket(ctx->socket, FIONBIO, &nb_mode);
             // if (select(ctx->socket + 1, &readfds, NULL, NULL, &tv) > 0) {
             //     char response[32];
             //     recv(ctx->socket, response, sizeof(response), 0); // Attempt to consume server close
             //     logToFile2("MWS: Consumed potential server CLOSE frame response.\n");
             // }
             // ioctlsocket(ctx->socket, FIONBIO, &b_mode);
        }
    } else if (ctx->state == WS_STATE_CLOSING) {
        logToFile2("MWS: ws_close called while already closing.\n");
    } else {
         logToFile2("MWS: ws_close called but state was not OPEN or CLOSING.\n");
    }

    // --- Force Close Section ---
    // This part executes regardless of the initial state or send success,
    // ensuring the socket is closed and resources are freed.

    // Perform TCP-level shutdown if socket is valid
    if (ctx->socket != INVALID_SOCKET) {
        logToFile2("MWS: Shutting down socket...\n");
        // Graceful shutdown attempt (SD_SEND) - ignore errors
        shutdown(ctx->socket, SD_SEND);

        // Optional: Consume remaining data to facilitate TCP close
        // char buffer[1024];
        // unsigned long nb_mode = 1, b_mode = 0;
        // ioctlsocket(ctx->socket, FIONBIO, &nb_mode);
        // while (recv(ctx->socket, buffer, sizeof(buffer), 0) > 0); // Consume loop
        // ioctlsocket(ctx->socket, FIONBIO, &b_mode);

        // Close the socket descriptor
        closesocket(ctx->socket);
        ctx->socket = INVALID_SOCKET;
        logToFile2("MWS: Socket closed.\n");
    }

    // Final state update
    ctx->state = WS_STATE_CLOSED;
    logToFile2("MWS: State set to CLOSED.\n");
    return 0;
}

//------------------------------------------------------------------------------
// Function: ws_fail_connection
//
// Initiates the closing handshake by sending a masked CLOSE frame (status code 1000)
// and then performs an orderly shutdown.
//------------------------------------------------------------------------------
int ws_fail_connection(ws_ctx* ctx, uint16_t status_code, const char* reason) {
    logToFile2("MWS: Failing WebSocket connection...\n");

    if (!ctx) {
        return -1;
    }

    if (ctx->state == WS_STATE_OPEN) {
        size_t reason_len = reason ? strlen(reason) : 0;
        size_t payload_len = 2 + reason_len;  // 2 bytes for status code, plus the reason text
        
        uint8_t* close_frame = (uint8_t*)malloc(6 + payload_len);
        if (!close_frame) {
            logToFile2("MWS: Memory allocation failed for close frame\n");
            goto force_close;
        }

        close_frame[0] = 0x88;  // FIN + CLOSE opcode
        close_frame[1] = 0x80 | (uint8_t)payload_len;  // Masked frame with payload length
        
        uint32_t mask = generate_mask();
        memcpy(close_frame + 2, &mask, 4);

        uint16_t net_code = htons(status_code);
        uint8_t* payload = close_frame + 6;
        memcpy(payload, &net_code, 2);

        if (reason_len > 0) {
            memcpy(payload + 2, reason, reason_len);
        }

        // Apply mask to the entire payload
        apply_mask(payload, payload_len, mask);

        send(ctx->socket, (char*)close_frame, 6 + payload_len, 0);
        free(close_frame);
    }

force_close:
    closesocket(ctx->socket);
    ctx->socket = INVALID_SOCKET;
    ctx->state = WS_STATE_CLOSED;

    logToFile2("MWS: WebSocket connection failed and closed\n");
    return 0;
}

//------------------------------------------------------------------------------
// Function: ws_destroy_ctx
//
// Frees the memory allocated for the WebSocket context.
//------------------------------------------------------------------------------
void ws_destroy_ctx(ws_ctx* ctx) {
    if (ctx) {
        if (ctx->recv_buffer) {
            free(ctx->recv_buffer);
        }
        free(ctx);
    }
}

//------------------------------------------------------------------------------
// Function: ws_cleanup
//
// Cleans up Winsock resources.
//------------------------------------------------------------------------------
void ws_cleanup(void) {
    WSACleanup();
}

//------------------------------------------------------------------------------
// Function: ws_get_state
//
// Returns the current state of the WebSocket connection.
//------------------------------------------------------------------------------
ws_state ws_get_state(ws_ctx* ctx) {
    return ctx->state;
}

//------------------------------------------------------------------------------
// Function: print_hex2
//
// Prints the contents of a buffer in hexadecimal format.
//------------------------------------------------------------------------------
void print_hex2(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        // For example, one might print: printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) { /* newline */ }
    }
    // End of hex dump.
}

//------------------------------------------------------------------------------
// Function: ws_check_server_available
//
// Performs a non-blocking TCP-level check to see whether the server is reachable.
// This function now monitors both the write and exception sets to detect errors.
//------------------------------------------------------------------------------
int ws_check_server_available(const char* host, int port) {
    logToFile2("MWS: Checking server availability...\n");
    
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
    
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int gai_result = getaddrinfo(host, port_str, &hints, &result);
    if (gai_result != 0) {
        char errMsg[256];
        snprintf(errMsg, sizeof(errMsg), "MWS: Failed to get address info: %s (Error: %d)\n", 
                gai_strerror(gai_result), gai_result);
        logToFile2(errMsg);
        return 0;
    }

    SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) {
        logToFile2("MWS: Failed to create socket\n");
        freeaddrinfo(result);
        return 0;
    }

    unsigned long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        logToFile2("MWS: Failed to set non-blocking mode\n");
        closesocket(sock);
        freeaddrinfo(result);
        return 0;
    }

    connect(sock, result->ai_addr, (int)result->ai_addrlen);
    
    fd_set write_fds, except_fds;
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);
    FD_SET(sock, &write_fds);
    FD_SET(sock, &except_fds);
    struct timeval timeout;
    timeout.tv_sec = 1;  // 1-second timeout
    timeout.tv_usec = 0;

    int available = 0;
    int select_ret = select(sock + 1, NULL, &write_fds, &except_fds, &timeout);
    if (select_ret > 0) {
        if (FD_ISSET(sock, &except_fds)) {
            logToFile2("MWS: Connection encountered an exception\n");
            available = 0;
        } else if (FD_ISSET(sock, &write_fds)) {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&error, &len) == 0 && error == 0) {
                logToFile2("MWS: Server is available\n");
                available = 1;
            } else {
                char errMsg[256];
                snprintf(errMsg, sizeof(errMsg), "Connection failed with error: %d\n", error);
                logToFile2(errMsg);
            }
        }
    } else {
        logToFile2("MWS: Connection attempt timed out\n");
    }

    closesocket(sock);
    freeaddrinfo(result);
    return available;
}

//------------------------------------------------------------------------------
// Function: ws_check_connection
//
// Checks whether the TCP connection is still alive. It first uses getsockopt()
// to check for socket errors and then employs a non-blocking peek on the socket.
//------------------------------------------------------------------------------
int ws_check_connection(ws_ctx* ctx) {
    if (ctx == NULL || ctx->socket == INVALID_SOCKET) {
        logToFile2("MWS: Invalid WebSocket context or socket.\n");
        return 0;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(ctx->socket, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err != 0) {
        logToFile2("MWS: Connection closed due to socket error.\n");
        return 0;
    }
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(ctx->socket, &read_fds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    int select_result = select(0, &read_fds, NULL, NULL, &tv);
    if (select_result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        char errMsg[256];
        snprintf(errMsg, sizeof(errMsg), "select() failed with error: %d\n", error);
        logToFile2(errMsg);
        return 0;
    }
    if (FD_ISSET(ctx->socket, &read_fds)) {
        char buffer;
        int recv_result = recv(ctx->socket, &buffer, 1, MSG_PEEK);
        if (recv_result == 0) {
            logToFile2("MWS: Connection has been closed by the server.\n");
            return 0;
        } else if (recv_result < 0) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
                return 1;
            } else {
                char errMsg[256];
                snprintf(errMsg, sizeof(errMsg), "recv() failed with error: %d\n", error);
                logToFile2(errMsg);
                return 0;
            }
        } else {
            return 1;
        }
    }
    return 1;
}

//------------------------------------------------------------------------------
// Function: ws_set_ping_pong
//
// Enables or disables automatic ping/pong handling for the WebSocket connection.
// When enabled (interval > 0), the library will automatically send ping frames
// at the specified interval and handle pong responses.
// When disabled (interval = 0), no automatic ping frames will be sent.
//
// Parameters:
//   ctx      - The WebSocket context
//   interval - Ping interval in seconds (0 to disable)
//
// Returns:
//   0 on success, -1 on failure
//------------------------------------------------------------------------------
int ws_set_ping_pong(ws_ctx* ctx, int interval) {
    if (!ctx) {
        logToFile2("MWS: Invalid context in ws_set_ping_pong\n");
        return -1;
    }
    
    ctx->ping_interval = interval;
    ctx->last_ping_time = time(NULL);
    
    char logMsg[100];
    if (interval > 0) {
        snprintf(logMsg, sizeof(logMsg), "MWS: Ping/pong enabled with %d second interval\n", interval);
    } else {
        snprintf(logMsg, sizeof(logMsg), "MWS: Ping/pong disabled\n");
    }
    logToFile2(logMsg);
    
    return 0;
}

//------------------------------------------------------------------------------
// Function: ws_recv
// Peeks, leaves control frames, consumes data frames (blocking reads).
//------------------------------------------------------------------------------
int ws_recv(ws_ctx* ctx, char* buffer, size_t buffer_size) {
    logToFile2("MWS: ws_recv attempting to receive data frame...\n");

    if (ctx->state != WS_STATE_OPEN) {
        logToFile2("MWS: ws_recv called but state is not OPEN.\n");
        return -1;
    }

    size_t total_received_in_buffer = 0;
    bool final_fragment = false;

    // Loop until a final data fragment is processed or the buffer is full
    while (!final_fragment && total_received_in_buffer < buffer_size) {
        
        // --- Peek at the next frame header ---
        uint8_t peek_header[2];
        int peek_bytes = recv(ctx->socket, (char*)peek_header, 2, MSG_PEEK);

        if (peek_bytes == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
                // No data available right now, return whatever we have accumulated (or 0)
                logToFile2("MWS: ws_recv peek WSAEWOULDBLOCK/WSAEINPROGRESS.\n");
                return total_received_in_buffer; 
            } else {
                char errMsg[256];
                snprintf(errMsg, sizeof(errMsg), "MWS: ws_recv peek failed: %d\n", error);
                logToFile2(errMsg);
                ws_close(ctx); // Close on socket error
                return -1;
            }
        } else if (peek_bytes == 0) {
            // Connection closed gracefully by peer during peek
            logToFile2("MWS: ws_recv peek detected connection closed by peer.\n");
            ws_close(ctx);
            return (total_received_in_buffer > 0) ? total_received_in_buffer : -1; // Return data if any, else -1
        } else if (peek_bytes != 2) {
            // Should not happen with TCP, but handle defensively
            logToFile2("MWS: ws_recv peek unexpected byte count.\n");
            ws_close(ctx);
            return -1;
        }

        // --- Check Opcode from Peek ---
        int opcode = peek_header[0] & 0x0F;
        logToFile2("MWS: ws_recv peeked opcode ");
        logToFileI2(opcode);

        // If it's a control frame, leave it for ws_service and return 0 (no app data received)
        if (opcode == WS_OPCODE_PING || opcode == WS_OPCODE_PONG || opcode == WS_OPCODE_CLOSE) {
            logToFile2("MWS: ws_recv peeked control frame. Returning 0, leaving for ws_service.\n");
            // Return any data already accumulated in buffer from previous fragments in *this call*
            return total_received_in_buffer; 
        }

        // --- It's a Data Frame (or unknown) - Consume and Process ---
        uint8_t actual_header[2];
        int bytes_read = recv(ctx->socket, (char*)actual_header, 2, 0); // Consume the peeked header
        
        if (bytes_read <= 0) { // Handle error or close that might occur between peek and read
            logToFile2("MWS: ws_recv error/close consuming header after peek.\n");
            ws_close(ctx);
             return (total_received_in_buffer > 0) ? total_received_in_buffer : -1;
        }

        final_fragment = (actual_header[0] & 0x80) != 0;
        opcode = actual_header[0] & 0x0F; // Re-read opcode from consumed header
        bool masked = (actual_header[1] & 0x80) != 0; 
        uint64_t payload_length = actual_header[1] & 0x7F;
        
        char logBuffer[256];
        snprintf(logBuffer, sizeof(logBuffer), "Frame Header (Consumed): final=%d, opcode=0x%X, masked=%d, len_indicator=%llu\n",
                final_fragment, opcode, masked, payload_length);
        logToFile2(logBuffer);

        if (masked) {
            logToFile2("MWS: Warning - Received masked frame from server (violates RFC 6455 Section 5.1).\n");
        }

        // Read extended payload length if necessary
        if (payload_length == 126) {
            uint16_t extended_length;
            bytes_read = recv(ctx->socket, (char*)&extended_length, 2, 0);
            if (bytes_read != 2) { logToFile2("MWS: Failed to read 16-bit ext len.\n"); ws_close(ctx); return -1; }
            payload_length = ntohs(extended_length);
        } else if (payload_length == 127) {
            uint64_t extended_length;
            bytes_read = recv(ctx->socket, (char*)&extended_length, 8, 0);
            if (bytes_read != 8) { logToFile2("MWS: Failed to read 64-bit ext len.\n"); ws_close(ctx); return -1; }
            payload_length = ntohll(extended_length);
        }

        // Read mask key if present
        uint32_t mask_key = 0;
        if (masked) {
            bytes_read = recv(ctx->socket, (char*)&mask_key, 4, 0);
            if (bytes_read != 4) { logToFile2("MWS: Failed to read mask key.\n"); ws_close(ctx); return -1; }
        }
        
        snprintf(logBuffer, sizeof(logBuffer), "Frame Details (Consumed): final=%d, opcode=0x%X, masked=%d, payload_length=%llu\n",
                final_fragment, opcode, masked, payload_length);
        logToFile2(logBuffer);

        // Process Data Frame (TEXT, BINARY, CONTINUATION)
        if (opcode == WS_OPCODE_TEXT || opcode == WS_OPCODE_BINARY || opcode == WS_OPCODE_CONTINUATION) {
             if (payload_length == 0) {
                 // Empty data frame
                 logToFile2("MWS: Consumed empty data frame.\n");
                 if (final_fragment) break; // If final empty fragment, we are done.
                 else continue; // If intermediate empty fragment, loop for next.
             }

             size_t remaining_buffer_space = buffer_size - total_received_in_buffer;
             size_t bytes_to_read_into_buffer = (payload_length < remaining_buffer_space) ? (size_t)payload_length : remaining_buffer_space;
             size_t bytes_to_discard = (payload_length > bytes_to_read_into_buffer) ? (size_t)(payload_length - bytes_to_read_into_buffer) : 0;

             // Read the data that fits into the buffer
             if (bytes_to_read_into_buffer > 0) {
                size_t current_payload_bytes_read = 0;
                while (current_payload_bytes_read < bytes_to_read_into_buffer) {
                    bytes_read = recv(ctx->socket, 
                                      buffer + total_received_in_buffer + current_payload_bytes_read, 
                                      bytes_to_read_into_buffer - current_payload_bytes_read, 
                                      0);
                    if (bytes_read <= 0) { // Error or connection closed during payload read
                        logToFile2("MWS: Error/close while reading data payload.\n");
                        ws_close(ctx);
                        return (total_received_in_buffer > 0) ? total_received_in_buffer : -1;
                    }
                    current_payload_bytes_read += bytes_read;
                }

                // Unmask the data *in the user buffer* if necessary
                if (masked) {
                    apply_mask((uint8_t*)(buffer + total_received_in_buffer), bytes_to_read_into_buffer, mask_key);
                }
                total_received_in_buffer += bytes_to_read_into_buffer;
             }

             // Discard any remaining payload that didn't fit
             if (bytes_to_discard > 0) {
                 logToFile2("MWS: Data frame payload exceeds buffer size. Discarding extra bytes.\n");
                 char discard_buf[1024];
                 uint64_t remaining_to_discard = bytes_to_discard;
                 while (remaining_to_discard > 0) {
                    size_t discard_now = (remaining_to_discard < sizeof(discard_buf)) ? (size_t)remaining_to_discard : sizeof(discard_buf);
                    bytes_read = recv(ctx->socket, discard_buf, discard_now, 0);
                    if (bytes_read <= 0) { // Error or connection closed during discard
                         logToFile2("MWS: Error/close while discarding excess data payload.\n");
                         ws_close(ctx);
                         return (total_received_in_buffer > 0) ? total_received_in_buffer : -1;
                    }
                    remaining_to_discard -= bytes_read;
                 }
             }
        } else {
            // Should not happen if peek check worked, but handle defensively
             logToFile2("MWS: Consumed frame with unexpected opcode after peek. Closing.\n");
             ws_close(ctx); // Initiate close on protocol error
             return -1;
        }
        
        // If the buffer is full but this wasn't the final fragment, break the loop.
        if (total_received_in_buffer >= buffer_size && !final_fragment) {
            logToFile2("MWS: Receive buffer full, but message is fragmented. Returning current data.\n");
            break; 
        }
    } // End while loop for fragments

    char logBuffer2[256];
    snprintf(logBuffer2, sizeof(logBuffer2), "MWS: ws_recv finished. Returning %zu bytes.\n", total_received_in_buffer);
    logToFile2(logBuffer2);
    return total_received_in_buffer; // Return the number of bytes actually placed in the buffer
}
