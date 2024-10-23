// Include necessary header files
#include "mws_lib.h"
#include "Logger2.h"

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

#define HEARTBEAT_INTERVAL 30 // 30 seconds
#define HEARTBEAT_TIMEOUT 10 // 10 seconds

// // WebSocket context structure
// struct ws_ctx {
//     SOCKET socket;        // Socket handle for the WebSocket connection
//     ws_state state;       // Current state of the WebSocket connection
//     char* recv_buffer;    // Buffer to store received data
//     size_t recv_buffer_size;  // Total size of the receive buffer
//     size_t recv_buffer_len;   // Current length of data in the receive buffer
// };

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
    logToFile2("Sending WebSocket handshake...\n");
    char key[16];
    char encoded_key[25];
    char request[1024];
    int request_len;

    // Generate random key
    logToFile2("Generating random key...\n");
    srand((unsigned int)time(NULL));
    for (int i = 0; i < 16; i++) {
        key[i] = rand() % 256;
    }
    char* base64_key = base64_encode(key, 16);
    strncpy_s(encoded_key, sizeof(encoded_key), base64_key, 24);
    encoded_key[24] = '\0';
    free(base64_key);
    logToFile2("Random key generated and encoded.\n");

    // Construct handshake request
    logToFile2("Constructing handshake request...\n");
    request_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, encoded_key);
    logToFile2("Handshake request constructed.\n");

    // Send handshake request
    logToFile2("Sending handshake request...\n");
    if (send(ctx->socket, request, request_len, 0) != request_len) {
        logToFile2("Failed to send handshake request.\n");
        return -1;
    }
    logToFile2("Handshake request sent successfully.\n");

    return 0;
}

// Function to parse WebSocket handshake response
static int ws_parse_handshake_response(ws_ctx* ctx) {
    logToFile2("Parsing WebSocket handshake response...\n");
    char buffer[1024];
    int bytes_received = recv(ctx->socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
        logToFile2("Failed to receive handshake response.\n");
        return -1;
    }
    buffer[bytes_received] = '\0';
    logToFile2("Received handshake response.\n");

    // Check for "HTTP/1.1 101" status
    if (strstr(buffer, "HTTP/1.1 101") == NULL) {
        logToFile2("Invalid handshake response: HTTP/1.1 101 not found.\n");
        return -1;
    }
    logToFile2("HTTP/1.1 101 status found in response.\n");

    // Check for "Upgrade: websocket"
    if (strstr(buffer, "Upgrade: websocket") == NULL) {
        logToFile2("Invalid handshake response: Upgrade: websocket not found.\n");
        return -1;
    }
    logToFile2("Upgrade: websocket found in response.\n");

    // TODO: Verify Sec-WebSocket-Accept if needed

    ctx->state = WS_STATE_OPEN;
    logToFile2("WebSocket connection established successfully.\n");
    return 0;
}

// Initialize Winsock
int ws_init(void) {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData);
}

// Create a new WebSocket context
ws_ctx* ws_create_ctx(void) {
    logToFile2("Creating WebSocket context...\n");
    ws_ctx* ctx = (ws_ctx*)malloc(sizeof(ws_ctx));
    if (ctx) {
        logToFile2("WebSocket context allocated successfully.\n");
        memset(ctx, 0, sizeof(ws_ctx));
        ctx->state = WS_STATE_CLOSED;
        ctx->recv_buffer_size = 1024;
        ctx->recv_buffer = (char*)malloc(ctx->recv_buffer_size);
        if (ctx->recv_buffer) {
            logToFile2("Receive buffer allocated successfully.\n");
            ctx->recv_buffer_len = 0;
        } else {
            logToFile2("Failed to allocate receive buffer.\n");
            free(ctx);
            return NULL;
        }
        logToFile2("WebSocket context initialized.\n");
    } else {
        logToFile2("Failed to allocate WebSocket context.\n");
    }
    return ctx;
}

// Add this helper function at the top of the file
static int try_connect_nonblocking(SOCKET sock, const struct addrinfo* addr, int timeout_sec) {
    // Set socket to non-blocking mode
    unsigned long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        logToFile2("Failed to set non-blocking mode\n");
        return -1;
    }

    // Attempt connection
    int result = connect(sock, addr->ai_addr, (int)addr->ai_addrlen);
    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            char errMsg[256];
            snprintf(errMsg, sizeof(errMsg), "Connect failed immediately with error: %d\n", error);
            logToFile2(errMsg);
            return -1;
        }

        // Connection is in progress, wait for completion
        fd_set write_fds, except_fds;
        struct timeval tv;
        FD_ZERO(&write_fds);
        FD_ZERO(&except_fds);
        FD_SET(sock, &write_fds);
        FD_SET(sock, &except_fds);
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;

        result = select(sock + 1, NULL, &write_fds, &except_fds, &tv);
        if (result == 0) {
            logToFile2("Connection attempt timed out\n");
            return -2;  // Special code for timeout
        }
        if (result == SOCKET_ERROR) {
            logToFile2("Select failed\n");
            return -1;
        }

        if (FD_ISSET(sock, &except_fds)) {
            logToFile2("Connection failed (exception)\n");
            return -1;
        }

        // Check if actually connected
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

    // Set socket back to blocking mode
    mode = 0;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        logToFile2("Failed to set blocking mode\n");
        return -1;
    }

    return 0;
}

// Modified ws_connect function
int ws_connect(ws_ctx* ctx, const char* uri) {
    logToFile2("Attempting to connect to WebSocket server\n");
    
    // Parse URI
    char schema[10], host[256], path[256];
    int port;
    if (sscanf_s(uri, "%9[^:]://%255[^:/]:%d%255s", schema, (unsigned)sizeof(schema), 
                 host, (unsigned)sizeof(host), &port, path, (unsigned)sizeof(path)) < 3) {
        if (sscanf_s(uri, "%9[^:]://%255[^/]%255s", schema, (unsigned)sizeof(schema), 
                     host, (unsigned)sizeof(host), path, (unsigned)sizeof(path)) < 3) {
            logToFile2("Failed to parse URI\n");
            return -1;
        }
        port = strcmp(schema, "wss") == 0 ? 443 : 80;
    }

    // Resolve address
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

    // Try each address until we successfully connect
    struct addrinfo *ptr;
    int connect_result = -1;
    for(ptr = addr_info; ptr != NULL; ptr = ptr->ai_next) {
        ctx->socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (ctx->socket == INVALID_SOCKET) {
            continue;
        }

        connect_result = try_connect_nonblocking(ctx->socket, ptr, 2); // 2 second timeout
        if (connect_result == 0) {
            break;  // Successfully connected
        }

        closesocket(ctx->socket);
        ctx->socket = INVALID_SOCKET;
    }

    freeaddrinfo(addr_info);

    if (connect_result != 0) {
        return -1;  // Failed to connect to any address
    }

    // Proceed with WebSocket handshake
    ctx->state = WS_STATE_CONNECTING;
    if (ws_send_handshake(ctx, host, path) != 0) {
        logToFile2("Failed to send WebSocket handshake\n");
        closesocket(ctx->socket);
        return -1;
    }

    if (ws_parse_handshake_response(ctx) != 0) {
        logToFile2("Failed to parse WebSocket handshake response\n");
        closesocket(ctx->socket);
        return -1;
    }

    logToFile2("WebSocket connection established successfully\n");
    return 0;
}

// Generate a random 32-bit mask for WebSocket frames
    // This function generates a random 32-bit mask for WebSocket frames
    // It combines four random numbers to create a single 32-bit value

    // Example:
    // Let's say rand() returns these values in sequence: 
    // 0x3A (58), 0xF2 (242), 0x7B (123), 0xC4 (196)
    // Step 1: 0x3A << 24 = 0x3A000000
    // Step 2: 0xF2 << 16 = 0x00F20000
    // Step 3: 0x7B << 8  = 0x00007B00
    // Step 4: 0xC4       = 0x000000C4
    // Combining these with bitwise OR:
    // 0x3A000000 | 0x00F20000 | 0x00007B00 | 0x000000C4 = 0x3AF27BC4
static uint32_t generate_mask() {

    return ((uint32_t)rand() << 24) | ((uint32_t)rand() << 16) | ((uint32_t)rand() << 8) | (uint32_t)rand();
}

// Send data over WebSocket
int ws_send(ws_ctx* ctx, const char* data, size_t length, int opcode) {
    char logBuffer[256];
    snprintf(logBuffer, sizeof(logBuffer), "Sending WebSocket frame: opcode=0x%X, length=%zu\n", opcode, length);
    logToFile2(logBuffer);

    // Check if the WebSocket connection is in the OPEN state
    if (ctx->state != WS_STATE_OPEN) {
        return -1;  // Return error if not in OPEN state
    }

    // Prepare variables for constructing the WebSocket frame
    uint8_t header[14];  // Maximum header size (2 + 8 + 4)
    size_t header_size = 2;  // Initial header size (FIN + opcode, and mask bit + payload length)
    uint32_t mask = generate_mask();  // Generate a random 32-bit mask

    // Set FIN bit (1) and opcode in the first byte of the header
    header[0] = 0x80 | (opcode & 0x0F);

    // Set the payload length and masking bit in the second byte (and potentially more)
    if (length <= 125) {
        header[1] = 0x80 | length;  // Use 7-bit length field
    }
    else if (length <= 65535) {
        header[1] = 0x80 | 126;  // Use 16-bit length field
        header[2] = (length >> 8) & 0xFF;  // High byte of 16-bit length
        header[3] = length & 0xFF;  // Low byte of 16-bit length
        header_size += 2;  // Increase header size for 16-bit length field
    }
    else {
        header[1] = 0x80 | 127;  // Use 64-bit length field
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (length >> ((7 - i) * 8)) & 0xFF;  // 64-bit length, byte by byte
        }
        header_size += 8;  // Increase header size for 64-bit length field
    }

    // Add the mask to the header
    memcpy(header + header_size, &mask, 4);
    header_size += 4;  // Increase header size for mask

    // Allocate memory for the entire frame (header + masked payload)
    size_t frame_size = header_size + length;
    uint8_t* frame = (uint8_t*)malloc(frame_size);
    if (!frame) return -1;  // Return error if memory allocation fails

    // Copy the header to the frame
    memcpy(frame, header, header_size);

    // Apply the mask to the data and copy it to the frame
    for (size_t i = 0; i < length; i++) {
        frame[header_size + i] = data[i] ^ ((uint8_t*)&mask)[i % 4];  // XOR each byte with the corresponding mask byte
    }

    // Debug: Print frame details
    // printf("Sending frame:\n");
    // printf("Opcode: 0x%02X\n", opcode);
    // printf("Mask: 0x%08X\n", mask);
    // printf("Payload length: %zu\n", length);
    // printf("Frame size: %zu\n", frame_size);
    // printf("Frame contents:\n");
    // print_hex2(frame, frame_size);  // Assuming print_hex2 is a custom function to print hex data

    // Send the entire frame
    int result = send(ctx->socket, (char*)frame, frame_size, 0);
    free(frame);  // Free the allocated memory for the frame

    // Check if the entire frame was sent successfully
    if (result != frame_size) {
        // printf("Send failed. Sent %d bytes out of %zu\n", result, frame_size);
        return -1;  // Return error if not all bytes were sent
    }

    logToFile2("WebSocket frame sent successfully\n");
    return 0;
}

// Receive data from WebSocket
int ws_recv(ws_ctx* ctx, char* buffer, size_t buffer_size) {
    logToFile2("Receiving WebSocket frame...\n");

    if (ctx->state != WS_STATE_OPEN) {
        // printf("Error: WebSocket not in OPEN state\n");
        return -1;
    }

    size_t total_received = 0;
    bool final_fragment = false;

    while (!final_fragment && total_received < buffer_size) {
        // Receive frame header
        uint8_t header[2];
        int bytes_received = recv(ctx->socket, (char*)header, 2, 0);
        if (bytes_received != 2) {
            // printf("Error receiving header: %d\n", WSAGetLastError());
            return -1;
        }

        // Parse frame header
        final_fragment = header[0] & 0x80;
        int opcode = header[0] & 0x0F;
        bool masked = header[1] & 0x80;
        uint64_t payload_length = header[1] & 0x7F;

        char logBuffer[256];
        snprintf(logBuffer, sizeof(logBuffer), "Frame info: final=%d, opcode=%d, masked=%d, payload_length=%llu\n",
            final_fragment, opcode, masked, payload_length);
        logToFile2(logBuffer);

        // Handle extended payload length
        if (payload_length == 126) {
            uint16_t extended_length;
            bytes_received = recv(ctx->socket, (char*)&extended_length, 2, 0);
            if (bytes_received != 2) {
                // printf("Error receiving extended length (16-bit): %d\n", WSAGetLastError());
                return -1;
            }
            payload_length = ntohs(extended_length);
        }
        else if (payload_length == 127) {
            uint64_t extended_length;
            bytes_received = recv(ctx->socket, (char*)&extended_length, 8, 0);
            if (bytes_received != 8) {
                // printf("Error receiving extended length (64-bit): %d\n", WSAGetLastError());
                return -1;
            }
            payload_length = ntohll(extended_length);
        }

        // printf("Actual payload length: %llu\n", payload_length);

        // Handle masking
        uint32_t mask = 0;
        if (masked) {
            bytes_received = recv(ctx->socket, (char*)&mask, 4, 0);
            if (bytes_received != 4) {
                // printf("Error receiving mask: %d\n", WSAGetLastError());
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
                if (bytes_received == 0) {
                    // printf("Connection closed by server\n");
                }
                else {
                    // printf("Error receiving payload: %d\n", WSAGetLastError());
                }
                return total_received > 0 ? total_received : -1;
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
                    // printf("Error discarding excess data: %d\n", WSAGetLastError());
                    break;
                }
                remaining -= bytes_received;
            }
            // printf("Discarded %llu bytes\n", payload_length - fragment_size);
        }
    }

    char logBuffer[256];
    snprintf(logBuffer, sizeof(logBuffer), "Received %zu bytes in total\n", total_received);
    logToFile2(logBuffer);
    return total_received;
}

// Close WebSocket connection
int ws_close(ws_ctx* ctx) {
    logToFile2("Closing WebSocket connection...\n");

    if (ctx->state == WS_STATE_OPEN) {
        // Send close frame
        uint8_t close_frame[] = { 0x88, 0x02, 0x03, 0xE8 }; // Status code 1000 (normal closure)
        int sent = send(ctx->socket, (char*)close_frame, sizeof(close_frame), 0);
        if (sent != sizeof(close_frame)) {
            // printf("Failed to send close frame\n");
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
                    // printf("Received close frame from server\n");
                    break;
                }
            }
        } while (recv_result > 0);

        if (recv_result <= 0) {
            // printf("Timeout or error while waiting for server's close frame\n");
        }
    }

    closesocket(ctx->socket);
    ctx->state = WS_STATE_CLOSED;
    logToFile2("WebSocket connection closed\n");
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

// Utility function to print data in hexadecimal format
void print_hex2(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        // printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) {} // printf("\n");
    }
    // printf("\n");
}

// New function to handle ping
static int ws_handle_ping(ws_ctx* ctx) {
    logToFile2("Handling ping frame...\n");
    uint8_t pong_frame[] = { 0x8A, 0x00 }; // Pong frame with no payload
    int sent = send(ctx->socket, (char*)pong_frame, sizeof(pong_frame), 0);
    if (sent != sizeof(pong_frame)) {
        // printf("Failed to send pong frame\n");
        return -1;
    }
    logToFile2("Pong frame sent\n");
    return 0;
}

// Modified ws_service function
int ws_service(ws_ctx* ctx) {
    logToFile2("Servicing WebSocket connection...\n");
    static time_t last_ping_time = 0;
    time_t current_time = time(NULL);

    // Check if it's time to send a ping
    if (current_time - last_ping_time >= HEARTBEAT_INTERVAL) {
        logToFile2("Sending ping frame...\n");
        uint8_t ping_frame[] = { 0x89, 0x00 }; // Ping frame with no payload
        int sent = send(ctx->socket, (char*)ping_frame, sizeof(ping_frame), 0);
        if (sent != sizeof(ping_frame)) {
            // printf("Failed to send ping frame\n");
            return -1;
        }
        last_ping_time = current_time;

        // Wait for pong response
        fd_set read_fds;
        struct timeval tv;
        FD_ZERO(&read_fds);
        FD_SET(ctx->socket, &read_fds);
        tv.tv_sec = HEARTBEAT_TIMEOUT;
        tv.tv_usec = 0;

        int select_result = select(ctx->socket + 1, &read_fds, NULL, NULL, &tv);
        if (select_result == -1) {
            // printf("Select error\n");
            return -1;
        }
        else if (select_result == 0) {
            logToFile2("Pong timeout\n");
            return -1;
        }

        // Receive the pong frame
        uint8_t header[2];
        int bytes_received = recv(ctx->socket, (char*)header, 2, 0);
        if (bytes_received != 2) {
            // printf("Error receiving pong header\n");
            return -1;
        }

        int opcode = header[0] & 0x0F;
        if (opcode != 0xA) { // 0xA is the opcode for pong
            char logBuffer[256];
            snprintf(logBuffer, sizeof(logBuffer), "Unexpected frame received (opcode: 0x%02X)\n", opcode);
            logToFile2(logBuffer);
            return -1;
        }
        logToFile2("Pong received successfully\n");
    }

    // Check for incoming frames
    fd_set read_fds;
    struct timeval tv;
    FD_ZERO(&read_fds);
    FD_SET(ctx->socket, &read_fds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    int select_result = select(ctx->socket + 1, &read_fds, NULL, NULL, &tv);
    if (select_result == -1) {
        // printf("Select error\n");
        return -1;
    }
    else if (select_result > 0) {
        uint8_t header[2];
        int bytes_received = recv(ctx->socket, (char*)header, 2, 0);
        if (bytes_received != 2) {
            // printf("Error receiving frame header\n");
            return -1;
        }

        int opcode = header[0] & 0x0F;
        if (opcode == 0x9) { // 0x9 is the opcode for ping
            return ws_handle_ping(ctx);
        }
        // Handle other frame types if needed
    }

    return 0;
}

// Function to check if server is available at TCP level
int ws_check_server_available(const char* host, int port) {
    logToFile2("Checking server availability...\n");
    
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
    
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        logToFile2("Failed to get address info\n");
        return 0;
    }

    SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) {
        logToFile2("Failed to create socket\n");
        freeaddrinfo(result);
        return 0;
    }

    // Set socket to non-blocking mode
    unsigned long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        logToFile2("Failed to set non-blocking mode\n");
        closesocket(sock);
        freeaddrinfo(result);
        return 0;
    }

    // Attempt to connect
    connect(sock, result->ai_addr, (int)result->ai_addrlen);
    
    fd_set write_fds;
    struct timeval timeout;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);
    timeout.tv_sec = 1;  // 1 second timeout
    timeout.tv_usec = 0;

    int available = 0;
    if (select(sock + 1, NULL, &write_fds, NULL, &timeout) == 1) {
        int error = 0;
        int len = sizeof(error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&error, &len) == 0 && error == 0) {
            logToFile2("Server is available\n");
            available = 1;
        } else {
            char errMsg[256];
            snprintf(errMsg, sizeof(errMsg), "Connection failed with error: %d\n", error);
            logToFile2(errMsg);
        }
    } else {
        logToFile2("Connection attempt timed out\n");
    }

    closesocket(sock);
    freeaddrinfo(result);
    return available;
}
