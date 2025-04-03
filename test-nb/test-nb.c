/*
 * ws_client.c
 *
 * A sample WebSocket client that uses the mws_lib library for client-side
 * WebSocket operations. This client checks every 2 seconds whether a server
 * is available at localhost on port 8765. Once available, it connects using
 * the URI "ws://localhost:8765/" and enters a communication loop.
 *
 * The communication loop does the following:
 *   - Calls ws_service() periodically to handle WebSocket control frames
 *     (ping/pong/close).
 *   - Receives any incoming data from the WebSocket.
 *   - Sends a test message every 10 seconds.
 *
 * The client stays connected until either the client or server closes the connection.
 *
 * This file is intended to be built using Visual Studio Code on Windows.
 */

// IMPORTANT: Define WIN32_LEAN_AND_MEAN before any Windows headers
#define WIN32_LEAN_AND_MEAN

// Include Windows headers first
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

// Then include standard C headers
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Finally include your custom headers
#include "mws_lib.h"
// Link with Ws2_32.lib
//#pragma comment(lib, "Ws2_32.lib")

int main(void) {
    // Seed the random generator once.
    srand((unsigned int)time(NULL));

    // Initialize Winsock. If this fails, exit.
    if (ws_init() != 0) {
        fprintf(stderr, "ws_client: Failed to initialize Winsock.\n");
        return 1;
    }
    printf("ws_client: Starting WebSocket client...\n");

    while (1) {
        // Check if the server is available on localhost at port 8765.
        if (ws_check_server_available("localhost", 8765)) {
            printf("ws_client: Server is available! Attempting to connect...\n");

            // Create a new WebSocket context.
            ws_ctx* ctx = ws_create_ctx();
            if (ctx == NULL) {
                printf("ws_client: Failed to allocate WebSocket context. Retrying...\n");
                Sleep(2000); // Wait 2 seconds before retrying.
                continue;
            }

            // Attempt to connect using the WebSocket URI.
            if (ws_connect(ctx, "ws://localhost:8765/") != 0) {
                printf("ws_client: Failed to connect to the server.\n");
                ws_destroy_ctx(ctx);
                Sleep(2000);
                continue;
            }
            printf("ws_client: Connected to WebSocket server at ws://localhost:8765/!\n");

            // Communication loop: as long as the connection remains open.
            time_t lastMsgTime = time(NULL);
            while (ws_get_state(ctx) == WS_STATE_OPEN) {
                // Process control frames (like PING, PONG, and CLOSE).
                ws_service(ctx);

                // Check for any incoming application data.
                char recvBuffer[1024];
                int bytesReceived = ws_recv(ctx, recvBuffer, sizeof(recvBuffer) - 1);
                if (bytesReceived > 0) {
                    // Null-terminate and print the received data.
                    recvBuffer[bytesReceived] = '\0';
                    printf("ws_client: Received: %s\n", recvBuffer);
                }

                // Every 10 seconds, send a test message.
                time_t currentTime = time(NULL);
                if (currentTime - lastMsgTime >= 10) {
                    const char* testMsg = "Hello from WebSocket client!";
                    if (ws_send(ctx, testMsg, strlen(testMsg), WS_OPCODE_TEXT) == 0) {
                        printf("ws_client: Sent: %s\n", testMsg);
                    } else {
                        printf("ws_client: Failed to send test message.\n");
                    }
                    lastMsgTime = currentTime;
                }

                // Sleep briefly (100 ms) so as to yield CPU time.
                Sleep(100);
            }

            // When the connection is no longer open, clean up the WebSocket context.
            printf("ws_client: Disconnected from server. Cleaning up context.\n");
            ws_destroy_ctx(ctx);
        } else {
            printf("ws_client: Server not available. Checking again in 2 seconds.\n");
        }
        // Wait 2 seconds before checking again.
        Sleep(2000);
    }

    // Cleanup Winsock resources (although this point will likely never be reached).
    ws_cleanup();
    return 0;
}