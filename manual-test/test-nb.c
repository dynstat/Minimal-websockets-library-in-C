#include "mws_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// Add these Windows networking headers in the correct order
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

// ANSI color codes for better visibility
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// Configuration
#define SERVER_HOST "localhost"
#define SERVER_PORT 8765
#define RECONNECT_INTERVAL 2000  // 2 seconds
#define MAX_BUFFER_SIZE 1024000  // 1MB

// Function to check if a server is available at the TCP level
static int check_server_available(const char* host, int port) {
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
    
    // Convert port number to string
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);

    // Get address information
    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        return 0; // Failed to get address info
    }

    // Create a socket
    SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(result);
        return 0; // Failed to create socket
    }

    // Set socket to non-blocking mode
    unsigned long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    // Attempt to connect
    connect(sock, result->ai_addr, (int)result->ai_addrlen);
    
    // Set up for select() to check connection status
    fd_set write_fds;
    struct timeval timeout;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);
    timeout.tv_sec = 1;  // 1 second timeout
    timeout.tv_usec = 0;

    int available = 0;
    // Check if the socket is ready for writing (connected) within the timeout period
    if (select(sock + 1, NULL, &write_fds, NULL, &timeout) == 1) {
        int error = 0;
        int len = sizeof(error);
        // Check if there's any error on the socket
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&error, &len);
        if (error == 0) {
            available = 1; // Connection successful
        }
    }

    // Clean up
    closesocket(sock);
    freeaddrinfo(result);
    return available;
}

// Function to handle WebSocket communication
static int handle_websocket_communication(ws_ctx* ctx) {
    // Send first message
    const char* message1 = "Hello, WebSocket!";
    printf(ANSI_COLOR_YELLOW "Sending: %s\n" ANSI_COLOR_RESET, message1);
    
    if (ws_send(ctx, message1, strlen(message1), WS_OPCODE_TEXT) != 0) {
        printf(ANSI_COLOR_RED "Failed to send first message\n" ANSI_COLOR_RESET);
        return -1;
    }

    // Receive echo response
    char recv_buffer[1024];
    int recv_len = ws_recv(ctx, recv_buffer, sizeof(recv_buffer));
    if (recv_len > 0) {
        recv_buffer[recv_len] = '\0';
        printf(ANSI_COLOR_GREEN "Received echo: %s\n" ANSI_COLOR_RESET, recv_buffer);
    } else {
        printf(ANSI_COLOR_RED "Failed to receive echo response\n" ANSI_COLOR_RESET);
        return -1;
    }

    // Receive additional message
    recv_len = ws_recv(ctx, recv_buffer, sizeof(recv_buffer));
    if (recv_len > 0) {
        recv_buffer[recv_len] = '\0';
        printf(ANSI_COLOR_GREEN "Received additional message: %s\n" ANSI_COLOR_RESET, recv_buffer);
    }

    // Send second message
    const char* message2 = "Thank you, server!";
    printf(ANSI_COLOR_YELLOW "Sending: %s\n" ANSI_COLOR_RESET, message2);
    
    if (ws_send(ctx, message2, strlen(message2), WS_OPCODE_TEXT) != 0) {
        printf(ANSI_COLOR_RED "Failed to send second message\n" ANSI_COLOR_RESET);
        return -1;
    }

    // Receive large response
    char* large_buffer = (char*)malloc(MAX_BUFFER_SIZE);
    if (!large_buffer) {
        printf(ANSI_COLOR_RED "Failed to allocate large buffer\n" ANSI_COLOR_RESET);
        return -1;
    }

    recv_len = ws_recv(ctx, large_buffer, MAX_BUFFER_SIZE - 1);
    if (recv_len > 0) {
        large_buffer[recv_len] = '\0';
        printf(ANSI_COLOR_GREEN "Received large response (length: %d)\n" ANSI_COLOR_RESET, recv_len);
        printf("First 50 characters: %.50s...\n", large_buffer);
    } else {
        printf(ANSI_COLOR_RED "Failed to receive large response\n" ANSI_COLOR_RESET);
        free(large_buffer);
        return -1;
    }

    free(large_buffer);
    return 0;
}

int main() {
    printf(ANSI_COLOR_BLUE "WebSocket Client Test Program\n" ANSI_COLOR_RESET);
    
    if (ws_init() != 0) {
        printf(ANSI_COLOR_RED "Failed to initialize WebSocket library\n" ANSI_COLOR_RESET);
        return -1;
    }

    ws_ctx* ctx = NULL;
    char uri[256];
    snprintf(uri, sizeof(uri), "ws://%s:%d", SERVER_HOST, SERVER_PORT);
    
    printf("Starting connection loop. Press Ctrl+C to exit.\n");

    while (1) {
        // Check server availability
        printf("Checking server availability...\n");
        if (!check_server_available(SERVER_HOST, SERVER_PORT)) {
            printf(ANSI_COLOR_YELLOW "Server not available. Retrying in %d ms...\n" ANSI_COLOR_RESET, 
                   RECONNECT_INTERVAL);
            Sleep(RECONNECT_INTERVAL);
            continue;
        }

        printf(ANSI_COLOR_GREEN "Server is available!\n" ANSI_COLOR_RESET);

        // Create WebSocket context if needed
        if (!ctx) {
            ctx = ws_create_ctx();
            if (!ctx) {
                printf(ANSI_COLOR_RED "Failed to create WebSocket context\n" ANSI_COLOR_RESET);
                ws_cleanup();
                return -1;
            }
        }

        // Attempt WebSocket connection
        printf("Attempting WebSocket connection to %s...\n", uri);
        if (ws_connect(ctx, uri) == 0) {
            printf(ANSI_COLOR_GREEN "WebSocket connected successfully!\n" ANSI_COLOR_RESET);
            
            // Handle WebSocket communication
            if (handle_websocket_communication(ctx) == 0) {
                printf(ANSI_COLOR_GREEN "Communication completed successfully\n" ANSI_COLOR_RESET);
            }

            // Service the connection
            while (ws_get_state(ctx) == WS_STATE_OPEN) {
                if (ws_service(ctx) != 0) {
                    printf(ANSI_COLOR_RED "Connection lost\n" ANSI_COLOR_RESET);
                    break;
                }
                Sleep(100);  // Small delay to prevent CPU overuse
            }

            // Close connection
            printf("Closing connection...\n");
            ws_close(ctx);
        } else {
            printf(ANSI_COLOR_RED "WebSocket connection failed\n" ANSI_COLOR_RESET);
        }

        // Cleanup and prepare for reconnection
        ws_destroy_ctx(ctx);
        ctx = NULL;
        printf("Waiting before reconnection attempt...\n");
        Sleep(RECONNECT_INTERVAL);
    }

    // Cleanup (this part won't be reached in this example)
    if (ctx) {
        ws_destroy_ctx(ctx);
    }
    ws_cleanup();

    return 0;
}
