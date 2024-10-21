#include "ws_lib.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

// ANSI color codes
#define ANSI_COLOR_GREEN  "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_RESET  "\x1b[0m"

int main() {
    if (ws_init() != 0) {
        printf("WebSocket initialization failed.\n");
        return -1;
    }

    ws_ctx* ctx = ws_create_ctx();
    if (!ctx) {
        printf("Failed to create WebSocket context.\n");
        ws_cleanup();
        return -1;
    }

    printf("Connecting to WebSocket server...\n");
    if (ws_connect(ctx, "ws://localhost:8765") != 0) {
        printf("WebSocket connection failed.\n");
        ws_destroy_ctx(ctx);
        ws_cleanup();
        return -1;
    }

    printf("WebSocket connected.\n");

    char recv_buffer[1024];
    int recv_len;

    // First message
    const char* message1 = "Hello, WebSocket!";
    printf("Sending message 1: ");
    printf(ANSI_COLOR_YELLOW "%s\n" ANSI_COLOR_RESET, message1);
    if (ws_send(ctx, message1, strlen(message1), WS_OPCODE_TEXT) != 0) {
        printf("Failed to send message 1.\n");
        goto cleanup;
    }
    printf("Message 1 sent successfully.\n");

    // Receive echo of first message
    printf("Waiting for echo of message 1...\n");
    recv_len = ws_recv(ctx, recv_buffer, sizeof(recv_buffer));
    if (recv_len > 0) {
        printf("Received echo: ");
        printf(ANSI_COLOR_GREEN "%.*s\n" ANSI_COLOR_RESET, recv_len, recv_buffer);
    } else {
        printf("Error receiving echo of message 1.\n");
        goto cleanup;
    }

    // Receive additional message from server
    printf("Waiting for additional message from server...\n");
    recv_len = ws_recv(ctx, recv_buffer, sizeof(recv_buffer));
    if (recv_len > 0) {
        printf("Received additional message: ");
        printf(ANSI_COLOR_GREEN "%.*s\n" ANSI_COLOR_RESET, recv_len, recv_buffer);
    } else {
        printf("Error receiving additional message.\n");
        goto cleanup;
    }

    // Second message
    const char* message2 = "Thank you, server!";
    printf("Sending message 2: ");
    printf(ANSI_COLOR_YELLOW "%s\n" ANSI_COLOR_RESET, message2);
    if (ws_send(ctx, message2, strlen(message2), WS_OPCODE_TEXT) != 0) {
        printf("Failed to send message 2.\n");
        goto cleanup;
    }
    printf("Message 2 sent successfully.\n");

    // Receive final response from server
    printf("Waiting for final response from server...\n");
    char* large_buffer = (char*)malloc(1000001); // Allocate 1MB + 1 byte for null terminator
    if (large_buffer == NULL) {
        printf("Failed to allocate memory for large buffer.\n");
        goto cleanup;
    }

    recv_len = ws_recv(ctx, large_buffer, 1000000);
    if (recv_len > 0) {
        large_buffer[recv_len] = '\0'; // Null-terminate the string
        printf("Received final response (length: %d): %.20s...\n", recv_len, large_buffer);
    } else {
        printf("Error receiving final response. recv_len = %d\n", recv_len);
        int error = WSAGetLastError();
        printf("WSA error code: %d\n", error);
        char error_msg[256];
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                      error_msg, sizeof(error_msg), NULL);
        printf("Error message: %s\n", error_msg);
    }

    free(large_buffer);

    // Add a small delay before closing the connection
    printf("Waiting before closing...\n");
    Sleep(1000); // Wait for 1 second

cleanup:
    printf("Closing WebSocket connection...\n");
    int close_result = ws_close(ctx);
    if (close_result == 0) {
        printf("WebSocket connection closed successfully.\n");
    } else {
        printf("Error closing WebSocket connection.\n");
    }

    ws_destroy_ctx(ctx);
    ws_cleanup();

    printf("WebSocket cleanup completed.\n");
    return 0;
}
