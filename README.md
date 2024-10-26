# Minimal WebSocket Library in C - mws_lib (TCP socket style without event loop)

A straightforward WebSocket client library implemented in C for Windows using Winsock2. This library provides a TCP socket-style interface for WebSocket communication, making it easy to use without requiring an additional event loop at the application layer.

## Features

- **Simplified Usage**: Offers a TCP socket-style interface for WebSocket operations.
- **Automatic WebSocket Handshake**: Handles the WebSocket handshake process internally.
- **Large Data Transfer**: Supports sending and receiving large data chunks efficiently.
- **Flexible Communication**: Allows sending and receiving messages from anywhere in your code.
- **Windows Compatibility**: Designed specifically for Windows platforms using Winsock2.

## Project Structure

This project is a Visual Studio solution containing two main components:

1. **mws_lib**: A static library project for the WebSocket implementation.
2. **test-mws**: A test project demonstrating the usage of the WebSocket library.

### Key Files

- `mws_lib.c` and `mws_lib.h`: Core implementation and header files for the WebSocket library.
- `test-mws/test-mws.c`: Example program showcasing the library's usage.
- `mws_lib.sln`: Visual Studio solution file.

## Build Instructions

### Prerequisites

- **Windows OS**: This library is specifically designed for Windows platforms.
- **Visual Studio**: Use a recent version of Visual Studio for building the project.

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

The library is designed to be simple and intuitive, similar to using standard TCP sockets. Key functions include:

- Initializing a WebSocket connection
- Sending data (text or binary)
- Receiving data
- Closing the connection

Refer to `test-mws/test-mws.c` for a detailed example of how to use the library in your application.

## Additional Files

- `server-2.py`: A Python script included for testing purposes (server-side implementation).

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
