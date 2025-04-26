# WebSocket Library in C

A simple WebSocket client library implemented in C for Windows using Winsock2. This library allows you to establish WebSocket connections, send and receive messages, and handle connection states.

## Features

- **WebSocket Handshake**: Establishes a WebSocket connection with the server.
- **Message Sending**: Supports sending text and binary messages.
- **Message Receiving**: Handles incoming messages from the server.
- **Connection Management**: Manages connection states and gracefully closes connections.

## Project Structure

This project is a Visual Studio solution containing two projects:

1. **mws_lib**: A static library project for the WebSocket implementation (Main Project).
2. **test-mws**: A test project to demonstrate the usage of the WebSocket library.

### Key Files

- `mws_lib.c` and `mws_lib.h`: Implementation and header files for the WebSocket library.
- `test-mws/test-mws.c`: Example program demonstrating how to use the WebSocket library.
- `mws_lib.sln`: Visual Studio solution file.

## Build Instructions

### Prerequisites

- **Windows OS**: The library is designed for Windows platforms.
- **Visual Studio**: This project uses Visual Studio for building. Ensure you have a recent version installed.

### Steps

1. **Open the Solution**
   - Open `mws_lib.sln` in Visual Studio.

2. **Build the Library**
   - Right-click on the `mws_lib` project in the Solution Explorer.
   - Select "Build" to compile the static library.

3. **Build the Test Program**
   - Right-click on the `test-mws` project in the Solution Explorer.
   - Select "Build" to compile the test program.

4. **Run the Test Program**
   - Right-click on the `test-mws` project.
   - Select "Set as Startup Project".
   - Press F5 or click "Start Debugging" to run the program.

## Usage

Refer to `test-mws/test-mws.c` for a comprehensive example of how to initialize the library, establish a connection, send and receive messages, and clean up resources.

## Additional Files

- `server-2.py`: A Python script, likely for testing purposes (server implementation).


## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
