/*
 * ws_client.c
 *
 * A sample WebSocket client that uses the mws_lib library for client-side
 * WebSocket operations. This client uses a separate thread to check server
 * availability and connects using the URI "ws://localhost:8765/" when available.
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
#include <process.h>  // For _beginthreadex

// Then include standard C headers
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

// Finally include your custom headers
#include "mws_lib.h"
// Link with Ws2_32.lib
//#pragma comment(lib, "Ws2_32.lib")

// Thread synchronization objects
HANDLE serverCheckThread = NULL;
CRITICAL_SECTION stateLock;
CONDITION_VARIABLE serverAvailableCV;
CONDITION_VARIABLE clientConnectedCV;

// Shared state variables
volatile bool serverAvailable = false;
volatile bool clientConnected = false;
volatile bool terminateThread = false;
volatile bool connectionFailed = false;  // New flag to indicate failed connection attempts

// Server check thread function
// The __stdcall calling convention is used for calling functions in the Windows API.
// It specifies how the function receives parameters from the caller and how the stack is cleaned up.
// In __stdcall, the callee cleans the stack, which can lead to more efficient code in certain scenarios.
// This calling convention is commonly used for Windows API functions and is defined in the Windows headers.
// The function name is also decorated with an underscore prefix and a trailing '@' followed by the number of bytes of parameters.
// Here, we define the serverCheckThreadFunc function using the __stdcall calling convention.
unsigned __stdcall serverCheckThreadFunc(void* arg) {
    const char* host = "localhost";
    int port = 8765;
    int retryDelay = 2000; // Start with 2 seconds

    while (!terminateThread) {
        // Check if client is connected - if so, wait until disconnected
        EnterCriticalSection(&stateLock);
        while (clientConnected && !terminateThread) {
            // Client is connected, wait for it to disconnect
            SleepConditionVariableCS(&clientConnectedCV, &stateLock, INFINITE);
        }
        
        // Check if connection failed - if so, reset server availability
        if (connectionFailed) {
            serverAvailable = false;
            connectionFailed = false;
        }
        LeaveCriticalSection(&stateLock);
        
        if (terminateThread) break;
        
        // Check server availability with exponential backoff
        while (!terminateThread) {
            EnterCriticalSection(&stateLock);
            bool isAvailable = serverAvailable;
            LeaveCriticalSection(&stateLock);
            
            // Skip check if we already know server is available
            if (isAvailable) break;
            
            if (ws_check_server_available(host, port)) {
                // Server is available
                EnterCriticalSection(&stateLock);
                serverAvailable = true;
                LeaveCriticalSection(&stateLock);
                
                // Signal main thread that server is available
                WakeConditionVariable(&serverAvailableCV);
                
                printf("Server check thread: Server is available!\n");
                break;
            }
            
            printf("Server check thread: Server not available. Retrying in %d ms...\n", retryDelay);
            Sleep(retryDelay);
            retryDelay = min(retryDelay * 2, 4000); // Exponential backoff, capped at 4 seconds
        }
        
        // Wait for client to connect or for thread termination
        EnterCriticalSection(&stateLock);
        while (serverAvailable && !clientConnected && !connectionFailed && !terminateThread) {
            // Server is available but client not yet connected
            SleepConditionVariableCS(&serverAvailableCV, &stateLock, INFINITE);
        }
        LeaveCriticalSection(&stateLock);
    }
    
    printf("Server check thread: Terminating\n");
    return 0;
}

int main(void) {
    // Seed the random generator once
    srand((unsigned int)time(NULL));

    // Initialize synchronization objects
    InitializeCriticalSection(&stateLock);
    InitializeConditionVariable(&serverAvailableCV);
    InitializeConditionVariable(&clientConnectedCV);

    // Initialize Winsock. If this fails, exit.
    if (ws_init() != 0) {
        fprintf(stderr, "ws_client: Failed to initialize Winsock.\n");
        return 1;
    }
    printf("ws_client: Starting WebSocket client...\n");

    // Start server check thread
    serverCheckThread = (HANDLE)_beginthreadex(NULL, 0, serverCheckThreadFunc, NULL, 0, NULL);
    if (serverCheckThread == NULL) {
        fprintf(stderr, "ws_client: Failed to create server check thread.\n");
        ws_cleanup();
        return 1;
    }

    while (1) {
        // Wait for server to become available
        EnterCriticalSection(&stateLock);
        while (!serverAvailable && !terminateThread) {
            printf("ws_client: Waiting for server to become available...\n");
            SleepConditionVariableCS(&serverAvailableCV, &stateLock, INFINITE);
        }
        LeaveCriticalSection(&stateLock);
        
        if (terminateThread) break;
        
        printf("ws_client: Server is available! Attempting to connect...\n");

        // Create a new WebSocket context
        ws_ctx* ctx = ws_create_ctx();
        if (ctx == NULL) {
            printf("ws_client: Failed to allocate WebSocket context. Retrying...\n");
            Sleep(2000);
            continue;
        }

        // Try to connect with exponential backoff
        int connectAttempts = 0;
        int maxConnectAttempts = 5;
        int connectRetryDelay = 2000; // Start with 2 seconds
        bool connected = false;
        
        while (connectAttempts < maxConnectAttempts) {
            if (ws_connect(ctx, "ws://localhost:8765/") == 0) {
                connected = true;
                break; // Successfully connected
            }
            
            connectAttempts++;
            if (connectAttempts >= maxConnectAttempts) {
                // Mark connection as failed after max attempts
                EnterCriticalSection(&stateLock);
                connectionFailed = true;
                LeaveCriticalSection(&stateLock);
                
                // Signal server check thread to re-verify server availability
                WakeConditionVariable(&serverAvailableCV);
                break;
            }
            
            printf("ws_client: Failed to connect to the server. Attempt %d of %d. Retrying in %d ms...\n",
                   connectAttempts, maxConnectAttempts, connectRetryDelay);
            Sleep(connectRetryDelay);
            connectRetryDelay = min(connectRetryDelay * 2, 4000); //  backoff
        }
        
        if (!connected) {
            printf("ws_client: Could not connect after %d attempts. Will check server availability again.\n", 
                   maxConnectAttempts);
            ws_destroy_ctx(ctx);
            continue;
        }
        
        printf("ws_client: Connected to WebSocket server at ws://localhost:8765/!\n");

        // Update connection state
        EnterCriticalSection(&stateLock);
        clientConnected = true;
        LeaveCriticalSection(&stateLock);
        
        // Communication loop: as long as the connection remains open
        time_t lastMsgTime = time(NULL);
        while (ws_get_state(ctx) == WS_STATE_OPEN) {
            // Process control frames (like PING, PONG, and CLOSE)
            if (ws_service(ctx) != 0 || !ws_check_connection(ctx)) {
                printf("ws_client: Connection issue detected during service.\n");
                break;
            }

            // Check for any incoming application data
            char recvBuffer[1024];
            int bytesReceived = ws_recv(ctx, recvBuffer, sizeof(recvBuffer) - 1);
            if (bytesReceived > 0) {
                // Null-terminate and print the received data
                recvBuffer[bytesReceived] = '\0';
                printf("ws_client: Received: %s\n", recvBuffer);
            } else if (bytesReceived < 0 && ws_get_state(ctx) == WS_STATE_OPEN) {
                // Only log errors if we're still supposed to be connected
                printf("ws_client: Error receiving data.\n");
            }

            // Every 10 seconds, send a test message
            time_t currentTime = time(NULL);
            if (currentTime - lastMsgTime >= 10) {
                const char* testMsg = "Hello from WebSocket client!";
                if (ws_send(ctx, testMsg, strlen(testMsg), WS_OPCODE_TEXT) == 0) {
                    printf("ws_client: Sent: %s\n", testMsg);
                } else {
                    printf("ws_client: Failed to send test message.\n");
                    break; // Exit the loop if sending fails
                }
                lastMsgTime = currentTime;
            }

            // Sleep briefly (100 ms) so as to yield CPU time
            Sleep(100);
        }

        // Implement graceful closing
        printf("ws_client: Connection ending. Sending close frame...\n");
        ws_close(ctx); // Send a proper close frame
        Sleep(500);    // Give time for the close handshake to complete
        
        printf("ws_client: Disconnected from server. Cleaning up context.\n");
        ws_destroy_ctx(ctx);
        
        // Update connection state
        EnterCriticalSection(&stateLock);
        clientConnected = false;
        LeaveCriticalSection(&stateLock);
        
        // Signal server check thread that client is disconnected
        WakeConditionVariable(&clientConnectedCV);
        
        // Wait a moment before attempting to reconnect
        Sleep(1000);
    }

    // Signal thread to terminate and wait for it
    terminateThread = true;
    WakeConditionVariable(&serverAvailableCV);
    WakeConditionVariable(&clientConnectedCV);
    WaitForSingleObject(serverCheckThread, INFINITE);
    CloseHandle(serverCheckThread);
    
    // Cleanup synchronization objects
    DeleteCriticalSection(&stateLock);
    
    // Cleanup Winsock resources
    ws_cleanup();
    return 0;
}