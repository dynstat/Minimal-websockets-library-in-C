# WebSocket Library in C

A simple WebSocket client library implemented in C for Windows using Winsock2. This library allows you to establish WebSocket connections, send and receive messages, and handle connection states.

## Features

- **WebSocket Handshake**: Establishes a WebSocket connection with the server.
- **Message Sending**: Supports sending text and binary messages.
- **Message Receiving**: Handles incoming messages from the server.
- **Connection Management**: Manages connection states and gracefully closes connections.
- **Hexadecimal Data Display**: Utility function to print data in hexadecimal format.

## Files

- [`ws_lib.c`](c:ws_lib.c): Implementation of the WebSocket library.
- [`ws_lib.h`](ws_lib.h): Header file for the WebSocket library.
- [`example_usage.c`](c:example_usage.c): Example program demonstrating how to use the WebSocket library.
- [`build_inst_ws_lib.c`](c:build_inst_ws_lib.c): Build instructions for compiling the library and example.

## Build Instructions

### Prerequisites

- **Visual Studio Code**: Ensure you have Visual Studio Code installed.
- **Windows OS**: The library is designed for Windows platforms.
- **C Compiler**: Microsoft Visual C++ (`cl.exe`) is required. Make sure it's available in your system's PATH.

### Steps

1. **Compile the WebSocket Library**

    ```bash
    cl /c ws_lib.c
    ```

    This command compiles `ws_lib.c` into an object file `ws_lib.obj`.

2. **Create the Static Library**

    ```bash
    lib ws_lib.obj
    ```

    This command creates a static library `ws_lib.lib` from the compiled object file.

3. **Compile the Example Usage Program**

    ```bash
    cl example_usage.c ws_lib.lib
    ```

    This command compiles `example_usage.c` and links it with `ws_lib.lib` to create the executable `example_usage.exe`.

4. **Run the Example**

    ```bash
    example_usage.exe
    ```

    Execute the compiled program to see the WebSocket client in action.

## Usage

Refer to [`example_usage.c`](c:example_usage.c) for a comprehensive example of how to initialize the library, establish a connection, send and receive messages, and clean up resources.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.