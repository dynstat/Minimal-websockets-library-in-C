# WebSocket Communication with Encryption

This project demonstrates a WebSocket-based communication system with encryption, featuring a C client and a Python server.

## Features

- Secure WebSocket communication
- AES-256 encryption for message exchange
- Client authentication and verification
- Heartbeat mechanism for connection monitoring

## Components

### Python Server (server.py)

- Implements the WebSocket server using `asyncio` and `websockets`
- Handles client authentication and name verification
- Processes encrypted messages and APDU commands
- Supports only localhost connections

### C Client (client.c)

- Uses `libwebsockets` for WebSocket communication
- Implements AES-256 encryption/decryption using OpenSSL
- Handles connection establishment and message exchange

## Setup

1. Install required libraries for both client and server
2. Compile the C client
3. Run the Python server
4. Connect using the C client

## Security Notes

- Uses a hardcoded encryption key (should be replaced with secure key management in production)
- Limited to localhost connections for added security

## Usage

Detailed usage instructions to be added.
