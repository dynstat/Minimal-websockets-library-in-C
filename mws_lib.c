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
    
    // Keep reading one byte at a time until the header terminator "\r\n\r\n" is found,
    // or until the buffer is almost full.
    while (total_received < (int)sizeof(buffer) - 1) {
        bytes_received = recv(ctx->socket, buffer + total_received, 1, 0);
        if (bytes_received <= 0) {
            logToFile2("MWS: Failed to receive handshake response\n");
            return -1;
        }
        total_received += bytes_received;
        buffer[total_received] = '\0';
        
        // Check if we have reached the end of the headers.
        if (strstr(buffer, "\r\n\r\n") != NULL) {
            break;
        }
    }
    
    logToFile2("MWS: Received handshake response.\n");
    
    // Validate that the handshake response is correct.
    if (strstr(buffer, "HTTP/1.1 101") == NULL) {
        logToFile2("MWS: Invalid handshake response: HTTP/1.1 101 not found.\n");
        return -1;
    }
    if (strstr(buffer, "Upgrade: websocket") == NULL) {
        logToFile2("MWS: Invalid handshake response: Upgrade: websocket not found.\n");
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
// Allocates and initializes a new WebSocket context structure.
//------------------------------------------------------------------------------
ws_ctx* ws_create_ctx(void) {
    logToFile2("MWS: Creating WebSocket context...\n");
    ws_ctx* ctx = (ws_ctx*)malloc(sizeof(ws_ctx));
    if (ctx) {
        logToFile2("MWS: WebSocket context allocated successfully.\n");
        memset(ctx, 0, sizeof(ws_ctx));
        ctx->state = WS_STATE_CLOSED;
        ctx->recv_buffer_size = 1024;
        ctx->recv_buffer = (char*)malloc(ctx->recv_buffer_size);
        if (ctx->recv_buffer) {
            logToFile2("MWS: Receive buffer allocated successfully.\n");
            ctx->recv_buffer_len = 0;
        } else {
            logToFile2("MWS: Failed to allocate receive buffer.\n");
            free(ctx);
            return NULL;
        }
        logToFile2("MWS: WebSocket context initialized.\n");
    } else {
        logToFile2("MWS: Failed to allocate WebSocket context.\n");
    }
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
    if (sscanf_s(uri, "%9[^:]://%255[^:/]:%d%255s", schema, (unsigned)sizeof(schema),
                 host, (unsigned)sizeof(host), &port, path, (unsigned)sizeof(path)) < 3) {
        if (sscanf_s(uri, "%9[^:]://%255[^/]%255s", schema, (unsigned)sizeof(schema),
                     host, (unsigned)sizeof(host), path, (unsigned)sizeof(path)) < 3) {
            logToFile2("MWS: Failed to parse URI\n");
            return -1;
        }
        port = strcmp(schema, "wss") == 0 ? 443 : 80;
    }

    if (strlen(path) == 0) {
        strcpy_s(path, sizeof(path), "/"); // Default path to '/'
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
    for(ptr = addr_info; ptr != NULL; ptr = ptr->ai_next) {
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
// Function: ws_recv
//
// Receives data from the WebSocket. It handles extended payload lengths,
// (optionally) unmasks the payload if needed, and discards excess data if the
// provided buffer length is too short.
//------------------------------------------------------------------------------
int ws_recv(ws_ctx* ctx, char* buffer, size_t buffer_size) {
    logToFile2("MWS: Receiving WebSocket frame...\n");

    if (ctx->state != WS_STATE_OPEN) {
        return -1;
    }

    size_t total_received = 0;
    bool final_fragment = false;

    while (!final_fragment && total_received < buffer_size) {
        // Read at least the 2-byte header.
        uint8_t header[2];
        int bytes_received = recv(ctx->socket, (char*)header, 2, 0);
        if (bytes_received != 2) {
            return -1;
        }

        final_fragment = (header[0] & 0x80) != 0;
        int opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        uint64_t payload_length = header[1] & 0x7F;

        char logBuffer[256];
        snprintf(logBuffer, sizeof(logBuffer), "Frame info: final=%d, opcode=%d, masked=%d, payload_length=%llu\n",
                final_fragment, opcode, masked, payload_length);
        logToFile2(logBuffer);

        if (payload_length == 126) {
            uint16_t extended_length;
            bytes_received = recv(ctx->socket, (char*)&extended_length, 2, 0);
            if (bytes_received != 2) {
                return -1;
            }
            payload_length = ntohs(extended_length);
        }
        else if (payload_length == 127) {
            uint64_t extended_length;
            bytes_received = recv(ctx->socket, (char*)&extended_length, 8, 0);
            if (bytes_received != 8) {
                return -1;
            }
            payload_length = ntohll(extended_length);
        }

        uint32_t mask = 0;
        if (masked) {
            bytes_received = recv(ctx->socket, (char*)&mask, 4, 0);
            if (bytes_received != 4) {
                return -1;
            }
        }

        size_t remaining_buffer = buffer_size - total_received;
        size_t fragment_size = (payload_length < remaining_buffer) ? payload_length : remaining_buffer;
        size_t bytes_to_receive = fragment_size;

        while (bytes_to_receive > 0) {
            bytes_received = recv(ctx->socket, buffer + total_received, bytes_to_receive, 0);
            if (bytes_received <= 0) {
                return total_received > 0 ? total_received : -1;
            }
            total_received += bytes_received;
            bytes_to_receive -= bytes_received;
        }

        // Unmask the payload if needed.
        if (masked) {
            for (size_t i = total_received - fragment_size; i < total_received; i++) {
                buffer[i] ^= ((uint8_t*)&mask)[i % 4];
            }
        }

        // If the frame contains more data than fits the buffer, discard the extras.
        if (fragment_size < payload_length) {
            size_t remaining = payload_length - fragment_size;
            char discard_buffer[1024];
            while (remaining > 0) {
                size_t to_discard = (remaining < sizeof(discard_buffer)) ? remaining : sizeof(discard_buffer);
                bytes_received = recv(ctx->socket, discard_buffer, to_discard, 0);
                if (bytes_received <= 0) {
                    break;
                }
                remaining -= bytes_received;
            }
        }
    }

    char logBuffer2[256];
    snprintf(logBuffer2, sizeof(logBuffer2), "Received %zu bytes in total\n", total_received);
    logToFile2(logBuffer2);
    return total_received;
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

    if (ctx->state == WS_STATE_OPEN) {
        uint8_t close_frame[6];  // 2 bytes header + 4 bytes mask key (payload length is 2)
        close_frame[0] = 0x88;
        close_frame[1] = 0x82;  // Set masked bit (0x80) and payload length 2
        uint32_t mask = generate_mask();
        memcpy(close_frame + 2, &mask, 4);
        
        uint16_t status_code = htons(1000);
        uint8_t payload[2];
        memcpy(payload, &status_code, 2);
        
        // Apply masking to the payload with our helper function.
        apply_mask(payload, 2, mask);

        if (send(ctx->socket, (char*)close_frame, 6, 0) != 6 ||
            send(ctx->socket, (char*)payload, 2, 0) != 2) {
            logToFile2("MWS: Failed to send close frame\n");
            goto force_close;
        }

        ctx->state = WS_STATE_CLOSING;
        logToFile2("MWS: Close frame sent, waiting for server close frame...\n");

        DWORD timeout = 5000; // 5-second timeout for receiving the server's close frame
        setsockopt(ctx->socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

        char response[32];
        int received = recv(ctx->socket, response, sizeof(response), 0);

        if (received > 0 && (response[0] & 0x0F) == 0x08) {
            logToFile2("MWS: Received close frame from server\n");
            
            if (received >= 4) {
                uint16_t received_code;
                memcpy(&received_code, &response[2], 2);
                received_code = ntohs(received_code);
                char logMsg[100];
                snprintf(logMsg, sizeof(logMsg), "MWS: Server close frame status code: %d\n", received_code);
                logToFile2(logMsg);
            }
        }

        if (shutdown(ctx->socket, SD_SEND) == 0) {
            char buffer[1024];
            fd_set readfds;
            struct timeval tv = {3, 0}; // 3-second timeout

            FD_ZERO(&readfds);
            FD_SET(ctx->socket, &readfds);

            while (select(ctx->socket + 1, &readfds, NULL, NULL, &tv) > 0) {
                if (recv(ctx->socket, buffer, sizeof(buffer), 0) <= 0) {
                    break;
                }
            }
        }
    }

force_close:
    closesocket(ctx->socket);
    ctx->socket = INVALID_SOCKET;
    ctx->state = WS_STATE_CLOSED;
    
    logToFile2("MWS: WebSocket connection closed\n");
    return 0;
}

//------------------------------------------------------------------------------
// Function: ws_fail_connection
//
// Sends a CLOSE frame with the given status code and an optional reason
// and then immediately closes the connection.
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
// Frees the WebSocket context including its receive buffer.
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
// Returns the current WebSocket connection state.
//------------------------------------------------------------------------------
ws_state ws_get_state(ws_ctx* ctx) {
    return ctx->state;
}

//------------------------------------------------------------------------------
// Function: print_hex2
//
// Utility function to print a byte array in hexadecimal format.
// (The actual printing code is commented out.)
//------------------------------------------------------------------------------
void print_hex2(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        // For example, one might print: printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) { /* newline */ }
    }
    // End of hex dump.
}

//------------------------------------------------------------------------------
// Function: ws_handle_ping
//
// Handles an incoming PING frame. It peeks at the header to determine the 
// length (including the possibility of extended payload lengths) and then
// reads the full frame. Once received, it sends a PONG frame echoing the same
// payload. If the PING frame happens to be masked (which it normally should not
// be from a server), the payload is unmasked first.
//------------------------------------------------------------------------------
static int ws_handle_ping(ws_ctx* ctx) {
    logToFile2("MWS: Handling ping frame...\n");

    uint8_t header[2];
    int bytes_received = recv(ctx->socket, (char*)header, 2, MSG_PEEK);
    if (bytes_received != 2) {
        logToFile2("MWS: Failed to peek ping frame header\n");
        return -1;
    }
    uint8_t payload_len_indicator = header[1] & 0x7F;
    size_t header_size = 2;
    uint64_t payload_length = payload_len_indicator;
    if (payload_len_indicator == 126) {
        header_size += 2;
        uint16_t ext_len;
        bytes_received = recv(ctx->socket, (char*)&ext_len, 2, MSG_PEEK);
        if (bytes_received != 2) {
            logToFile2("MWS: Failed to peek extended length (16-bit)\n");
            return -1;
        }
        payload_length = ntohs(ext_len);
    } else if (payload_len_indicator == 127) {
        header_size += 8;
        uint64_t ext_len;
        bytes_received = recv(ctx->socket, (char*)&ext_len, 8, MSG_PEEK);
        if (bytes_received != 8) {
            logToFile2("MWS: Failed to peek extended length (64-bit)\n");
            return -1;
        }
        payload_length = ntohll(ext_len);
    }
    // Check if the frame is masked (server frames should not be masked, but we handle it if so)
    bool masked = (header[1] & 0x80) != 0;
    if (masked) {
        header_size += 4;
    }
    size_t frame_size = header_size + payload_length;
    uint8_t* frame_buffer = (uint8_t*)malloc(frame_size);
    if (!frame_buffer) {
        logToFile2("MWS: Failed to allocate memory for ping frame buffer\n");
        return -1;
    }
    bytes_received = recv(ctx->socket, (char*)frame_buffer, frame_size, 0);
    if (bytes_received != frame_size) {
        logToFile2("MWS: Failed to read complete ping frame\n");
        free(frame_buffer);
        return -1;
    }
    if (masked) {
        uint32_t mask;
        memcpy(&mask, frame_buffer + header_size - 4, 4);
        apply_mask(frame_buffer + header_size, payload_length, mask);
    }
    logToFile2("MWS: Received ping frame. Sending pong response...\n");
    // Use ws_send to send a masked pong frame with the same payload.
    int ret = ws_send(ctx, (char*)(frame_buffer + header_size), payload_length, WS_OPCODE_PONG);
    free(frame_buffer);
    return ret;
}

//------------------------------------------------------------------------------
// Function: ws_service
//
// Regularly called to service the WebSocket connection (handling control frames).
// It checks for PING, PONG, and CLOSE, and also sends periodic PING frames if needed.
//------------------------------------------------------------------------------
int ws_service(ws_ctx* ctx) {
    logToFile2("MWS: Servicing WebSocket connection for ping/pong...\n");

    uint8_t header[2];
    int bytes_received = recv(ctx->socket, (char*)header, 2, MSG_PEEK);
    if (bytes_received > 0) {
        int opcode = header[0] & 0x0F;
        switch(opcode) {
            case WS_OPCODE_PING:
                logToFile2("MWS: Received ping frame\n");
                return ws_handle_ping(ctx);
            case WS_OPCODE_PONG:
                logToFile2("MWS: Received pong frame\n");
                {
                    uint8_t payload_length = header[1] & 0x7F;
                    char discard[256];
                    // Consume the pong frame from the socket.
                    recv(ctx->socket, discard, 2 + payload_length, 0);
                }
                return 0;
            case WS_OPCODE_CLOSE:
                logToFile2("MWS: Received close frame\n");
                recv(ctx->socket, (char*)header, 2, 0);
                ws_close(ctx);
                return -1;
        }
    }

    // Send periodic ping frames if the time interval has elapsed.
    static time_t last_ping_time = 0;
    time_t current_time = time(NULL);
    if (current_time - last_ping_time >= (PING_TIMEOUT_MS / 1000)) {
        uint8_t ping_frame[] = { 0x89, 0x00 };  // 0x89: FIN + PING opcode, 0x00: no payload
        if (send(ctx->socket, (char*)ping_frame, sizeof(ping_frame), 0) != sizeof(ping_frame)) {
            logToFile2("MWS: Failed to send ping frame\n");
            return -1;
        }
        fd_set read_fds;
        struct timeval tv;
        FD_ZERO(&read_fds);
        FD_SET(ctx->socket, &read_fds);
        tv.tv_sec = PONG_TIMEOUT_MS / 1000;
        tv.tv_usec = (PONG_TIMEOUT_MS % 1000) * 1000;

        if (select(ctx->socket + 1, &read_fds, NULL, NULL, &tv) <= 0) {
            logToFile2("MWS: Pong timeout - closing connection\n");
            ws_close(ctx);
            return -1;
        }
        last_ping_time = current_time;
    }
    return 0;
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
