import asyncio
import websockets
from colorama import Fore, Style, init
import struct
import time

# Initialize colorama
init(autoreset=True)

break_loop = False


async def handle_client(websocket: websockets.WebSocketServerProtocol):
    global break_loop
    # Set shorter ping interval for testing
    websocket.ping_interval = None  # Disable automatic pings
    websocket.ping_timeout = None   # Disable ping timeout
    
    print(f"New connection from {websocket.remote_address}")
    
    try:
        while not break_loop:
            # Receive binary message from client
            message = await websocket.recv()
            if message is None:
                break

            if isinstance(message, bytes):
                # Extract tag from the message (second byte)
                tag_recvd = message[1] if len(message) > 1 else None
                
                print(f"RECVD from client: {' '.join([f'{b:02X}' for b in message])}")

                # Handle different tags
                if tag_recvd == 91:  # Status Request
                    print("Status Request received")
                    random_data = b"\x55\x55\x55\x55"
                    response = (
                        tag_recvd.to_bytes(2, byteorder='big') +
                        len(random_data).to_bytes(2, byteorder='big') +
                        random_data
                    )
                    await websocket.send(response)
                    print(f"SENT to Client: STATUS RESPONSE - {' '.join([f'{b:02X}' for b in response])}\n")

                elif tag_recvd == 87:  # ATR Request
                    atr = b"\x3f\x95\x13\x81\x01\x80\x73\xff\x01\x00\x0b"
                    tag_sent = 88
                    response = (
                        tag_sent.to_bytes(2, byteorder='big') +
                        len(atr).to_bytes(2, byteorder='big') +
                        atr
                    )
                    await websocket.send(response)
                    print(f"SENT to Client: ATR - {' '.join([f'{b:02X}' for b in response])}\n")

                elif tag_recvd == 89:  # Command APDU
                    resp = b"\x6d\x82"
                    tag_sent = 90
                    response = (
                        tag_sent.to_bytes(2, byteorder='big') +
                        len(resp).to_bytes(2, byteorder='big') +
                        resp
                    )
                    await websocket.send(response)
                    print(f"SENT to Client ({len(resp)}): {' '.join([f'{b:02X}' for b in response])}\n")

                elif tag_recvd in [83, 81]:  # Special tags
                    print("\nRECEIVED TAG: 81 or 83 (PASSED)\n")
                    continue

                elif tag_recvd == 85:  # Reset
                    continue

                else:  # Default response
                    response = b'\x00\x02\x6d\x82'
                    await websocket.send(response)
                    print(f"SENT to Client: {' '.join([f'{b:02X}' for b in response])}\n")

    except websockets.exceptions.ConnectionClosedError as e:
        print(f"\nConnection closed: {e}\n")

async def main():
    global break_loop
    server = await websockets.serve(
        handle_client,
        "localhost",
        8765,
        max_size=None  # Allow unlimited message size
    )
    
    print("WebSocket Server started on ws://localhost:8765")
    print("Press Ctrl+C to stop.")
    
    try:
        await server.wait_closed()
    except asyncio.CancelledError:
        print("Server is shutting down...")
        break_loop = True
    finally:
        server.close()
        await server.wait_closed()
        print("Server has been shut down.")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nKeyboard interrupt received. Shutting down...")