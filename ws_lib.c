// Include necessary header files
#include "ws_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Windows-specific includes and definitions
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

// WebSocket-specific constants
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_HEADER_SIZE 14

// WebSocket context structure
struct ws_ctx {
    SOCKET socket;
    ws_state state;
    char* recv_buffer;
    size_t recv_buffer_size;
    size_t recv_buffer_len;
};

// Base64 encoding table
static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Function to encode data in base64
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

    // Add padding if necessary
    for (i = 0; i < (3 - length % 3) % 3; i++)
        encoded[output_length - 1 - i] = '=';

    encoded[output_length] = '\0';

    return encoded;
}

// Function to send WebSocket handshake
static int ws_send_handshake(ws_ctx* ctx, const char* host, const char* path) {
    char key[16];
    char encoded_key[25];
    char request[1024];
    int request_len;

    // Generate random key
    srand((unsigned int)time(NULL));
    for (int i = 0; i < 16; i++) {
        key[i] = rand() % 256;
    }
    char* base64_key = base64_encode(key, 16);
    strncpy(encoded_key, base64_key, 24);
    encoded_key[24] = '\0';
    free(base64_key);

    // Construct handshake request
    request_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, encoded_key);

    // Send handshake request
    if (send(ctx->socket, request, request_len, 0) != request_len) {
        return -1;
    }

    return 0;
}

// Function to parse WebSocket handshake response
static int ws_parse_handshake_response(ws_ctx* ctx) {
    char buffer[1024];
    int bytes_received = recv(ctx->socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
        return -1;
    }
    buffer[bytes_received] = '\0';

    // Check for "HTTP/1.1 101" status
    if (strstr(buffer, "HTTP/1.1 101") == NULL) {
        return -1;
    }

    // Check for "Upgrade: websocket"
    if (strstr(buffer, "Upgrade: websocket") == NULL) {
        return -1;
    }

    // TODO: Verify Sec-WebSocket-Accept if needed

    ctx->state = WS_STATE_OPEN;
    return 0;
}

// Initialize Winsock
int ws_init(void) {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData);
}

// Create a new WebSocket context
ws_ctx* ws_create_ctx(void) {
    ws_ctx* ctx = (ws_ctx*)malloc(sizeof(ws_ctx));
    if (ctx) {
        memset(ctx, 0, sizeof(ws_ctx));
        ctx->state = WS_STATE_CLOSED;
        ctx->recv_buffer_size = 1024;
        ctx->recv_buffer = (char*)malloc(ctx->recv_buffer_size);
        ctx->recv_buffer_len = 0;
    }
    return ctx;
}

// Connect to a WebSocket server
int ws_connect(ws_ctx* ctx, const char* uri) {
    // Parse URI
    char schema[10], host[256], path[256];
    int port;
    if (sscanf(uri, "%[^:]://%[^:/]:%d%s", schema, host, &port, path) < 3) {
        if (sscanf(uri, "%[^:]://%[^/]%s", schema, host, path) < 3) {
            return -1;
        }
        port = strcmp(schema, "wss") == 0 ? 443 : 80;
    }
    if (path[0] == '\0') {
        strcpy(path, "/");
    }

    // Resolve address
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    // Convert the integer port number to a string
    char port_str[6];  // Buffer to hold the port number as a string
    // Use snprintf to safely convert the port number to a string
    // Example: If port is 8080, port_str will be "8080\0"
    //          If port is 80, port_str will be "80\0"
    snprintf(port_str, sizeof(port_str), "%d", port);
    // Note: sizeof(port_str) is 6, which allows for up to 5 digits plus null terminator
    // This covers all valid port numbers (0-65535)

    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        return -1;
    }

    // Create socket and connect
    ctx->socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (ctx->socket == INVALID_SOCKET) {
        freeaddrinfo(result);
        return -1;
    }

    if (connect(ctx->socket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        closesocket(ctx->socket);
        freeaddrinfo(result);
        return -1;
    }

    freeaddrinfo(result);

    // Send WebSocket handshake
    ctx->state = WS_STATE_CONNECTING;
    if (ws_send_handshake(ctx, host, path) != 0) {
        closesocket(ctx->socket);
        return -1;
    }

    // Parse handshake response
    if (ws_parse_handshake_response(ctx) != 0) {
        closesocket(ctx->socket);
        return -1;
    }

    return 0;
}

// Generate a random 32-bit mask for WebSocket frames
static uint32_t generate_mask() {
    return ((uint32_t)rand() << 24) | ((uint32_t)rand() << 16) | ((uint32_t)rand() << 8) | (uint32_t)rand();
}

// Send data over WebSocket
int ws_send(ws_ctx* ctx, const char* data, size_t length, int opcode) {
    if (ctx->state != WS_STATE_OPEN) {
        return -1;
    }

    // Construct WebSocket frame
    uint8_t header[14];  // Maximum header size (2 + 8 + 4)
    size_t header_size = 2;
    uint32_t mask = generate_mask();

    // Set FIN bit and opcode
    header[0] = 0x80 | (opcode & 0x0F);
    
    // Set payload length and masking bit
    if (length <= 125) {
        header[1] = 0x80 | length;
    } else if (length <= 65535) {
        header[1] = 0x80 | 126;
        header[2] = (length >> 8) & 0xFF;
        header[3] = length & 0xFF;
        header_size += 2;
    } else {
        header[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (length >> ((7 - i) * 8)) & 0xFF;
        }
        header_size += 8;
    }

    // Add mask to header
    memcpy(header + header_size, &mask, 4);
    header_size += 4;

    // Allocate buffer for entire frame (header + masked payload)
    size_t frame_size = header_size + length;
    uint8_t* frame = (uint8_t*)malloc(frame_size);
    if (!frame) return -1;

    // Copy header to frame
    memcpy(frame, header, header_size);

    // Apply mask to data and copy to frame
    for (size_t i = 0; i < length; i++) {
        frame[header_size + i] = data[i] ^ ((uint8_t*)&mask)[i % 4];
    }

    // Debug: Print frame details
    printf("Sending frame:\n");
    printf("Opcode: 0x%02X\n", opcode);
    printf("Mask: 0x%08X\n", mask);
    printf("Payload length: %zu\n", length);
    printf("Frame size: %zu\n", frame_size);
    printf("Frame contents:\n");
    print_hex2(frame, frame_size);

    // Send entire frame
    int result = send(ctx->socket, (char*)frame, frame_size, 0);
    free(frame);

    if (result != frame_size) {
        printf("Send failed. Sent %d bytes out of %zu\n", result, frame_size);
        return -1;
    }

    return 0;
}

// Receive data from WebSocket
int ws_recv(ws_ctx* ctx, char* buffer, size_t buffer_size) {
    if (ctx->state != WS_STATE_OPEN) {
        printf("Error: WebSocket not in OPEN state\n");
        return -1;
    }

    size_t total_received = 0;
    bool final_fragment = false;

    while (!final_fragment && total_received < buffer_size) {
        // Receive frame header
        uint8_t header[2];
        int bytes_received = recv(ctx->socket, (char*)header, 2, 0);
        if (bytes_received != 2) {
            printf("Error receiving header: %d\n", WSAGetLastError());
            return -1;
        }

        // Parse frame header
        final_fragment = header[0] & 0x80;
        int opcode = header[0] & 0x0F;
        bool masked = header[1] & 0x80;
        uint64_t payload_length = header[1] & 0x7F;

        printf("Frame info: final=%d, opcode=%d, masked=%d, payload_length=%llu\n", 
               final_fragment, opcode, masked, payload_length);

        // Handle extended payload length
        if (payload_length == 126) {
            uint16_t extended_length;
            bytes_received = recv(ctx->socket, (char*)&extended_length, 2, 0);
            if (bytes_received != 2) {
                printf("Error receiving extended length (16-bit): %d\n", WSAGetLastError());
                return -1;
            }
            payload_length = ntohs(extended_length);
        } else if (payload_length == 127) {
            uint64_t extended_length;
            bytes_received = recv(ctx->socket, (char*)&extended_length, 8, 0);
            if (bytes_received != 8) {
                printf("Error receiving extended length (64-bit): %d\n", WSAGetLastError());
                return -1;
            }
            payload_length = ntohll(extended_length);
        }

        printf("Actual payload length: %llu\n", payload_length);

        // Handle masking
        uint32_t mask = 0;
        if (masked) {
            bytes_received = recv(ctx->socket, (char*)&mask, 4, 0);
            if (bytes_received != 4) {
                printf("Error receiving mask: %d\n", WSAGetLastError());
                return -1;
            }
        }

        // Receive payload data
        size_t remaining_buffer = buffer_size - total_received;
        size_t fragment_size = (payload_length < remaining_buffer) ? payload_length : remaining_buffer;
        size_t bytes_to_receive = fragment_size;

        while (bytes_to_receive > 0) {
            bytes_received = recv(ctx->socket, buffer + total_received, bytes_to_receive, 0);
            if (bytes_received <= 0) {
                printf("Error receiving payload: expected %zu, got %d\n", bytes_to_receive, bytes_received);
                return -1;
            }
            total_received += bytes_received;
            bytes_to_receive -= bytes_received;
        }

        // Unmask data if necessary
        if (masked) {
            for (size_t i = total_received - fragment_size; i < total_received; i++) {
                buffer[i] ^= ((uint8_t*)&mask)[i % 4];
            }
        }

        // Discard excess data if buffer is full
        if (fragment_size < payload_length) {
            size_t remaining = payload_length - fragment_size;
            char discard_buffer[1024];
            while (remaining > 0) {
                size_t to_discard = (remaining < sizeof(discard_buffer)) ? remaining : sizeof(discard_buffer);
                bytes_received = recv(ctx->socket, discard_buffer, to_discard, 0);
                if (bytes_received <= 0) {
                    printf("Error discarding excess data: %d\n", WSAGetLastError());
                    break;
                }
                remaining -= bytes_received;
            }
            printf("Discarded %llu bytes\n", payload_length - fragment_size);
        }
    }

    printf("Total received: %zu bytes\n", total_received);
    return total_received;
}

// Close WebSocket connection
int ws_close(ws_ctx* ctx) {
    if (ctx->state == WS_STATE_OPEN) {
        // Send close frame
        uint8_t close_frame[] = {0x88, 0x02, 0x03, 0xE8}; // Status code 1000 (normal closure)
        int sent = send(ctx->socket, (char*)close_frame, sizeof(close_frame), 0);
        if (sent != sizeof(close_frame)) {
            printf("Failed to send close frame\n");
            return -1;
        }
        ctx->state = WS_STATE_CLOSING;
        
        // Wait for the server's close frame (with a timeout)
        char buffer[1024];
        int recv_result;
        struct timeval tv;
        tv.tv_sec = 5;  // 5 seconds timeout
        tv.tv_usec = 0;
        setsockopt(ctx->socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        
        do {
            recv_result = recv(ctx->socket, buffer, sizeof(buffer), 0);
            if (recv_result > 0) {
                if ((buffer[0] & 0x0F) == 0x8) {
                    printf("Received close frame from server\n");
                    break;
                }
            }
        } while (recv_result > 0);

        if (recv_result <= 0) {
            printf("Timeout or error while waiting for server's close frame\n");
        }
    }
    
    closesocket(ctx->socket);
    ctx->state = WS_STATE_CLOSED;
    return 0;
}

// Destroy WebSocket context
void ws_destroy_ctx(ws_ctx* ctx) {
    if (ctx) {
        if (ctx->recv_buffer) {
            free(ctx->recv_buffer);
        }
        free(ctx);
    }
}

// Clean up Winsock
void ws_cleanup(void) {
    WSACleanup();
}

// Get current WebSocket state
ws_state ws_get_state(ws_ctx* ctx) {
    return ctx->state;
}

// Service WebSocket connection (placeholder for future implementation)
int ws_service(ws_ctx* ctx) {
    // TODO: Implement ping/pong, handle incoming frames, etc.
    return 0;
}

// Utility function to print data in hexadecimal format
void print_hex2(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");
}
