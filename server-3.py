# Import the asyncio library, which provides infrastructure for writing asynchronous code
import asyncio
# Import the websockets library, which implements the WebSocket protocol
import websockets
# Import colorama for colored terminal output
from colorama import Fore, Style, init
import struct
import time  # Add this import

# Initialize colorama
init(autoreset=True)

# Define break_loop as a global variable
break_loop = False

# Define an asynchronous function named 'echo' that handles WebSocket connections
# The 'async' keyword indicates that this function can be paused and resumed
async def echo(websocket: websockets.WebSocketServerProtocol):
    global break_loop  # Add this line to access the global variable
    print(f"New connection: with address {websocket.remote_address} and port {websocket.port}")
    
    # Override the default ping method
    original_ping = websocket.ping
    async def ping_wrapper(*args, **kwargs):
        print(f"\n{Fore.MAGENTA}Sending PING to client{Style.RESET_ALL}")
        ping_data = struct.pack('!Q', time.time_ns())
        print(f"""
{Fore.BLUE}PING Frame Details:{Style.RESET_ALL}
    FIN: True
    RSV1: False
    RSV2: False
    RSV3: False
    Opcode: PING (0x09)
    Masked: False
    Payload length: {len(ping_data)} bytes
    Payload (hex): {ping_data.hex()}
    Raw frame bytes: 0x89 {len(ping_data):02x} {ping_data.hex()}
        """)
        return await original_ping(ping_data)

    # Override the default pong handler
    original_pong = websocket.pong
    async def pong_wrapper(*args, **kwargs):
        print(f"\n{Fore.YELLOW}Sending PONG to client{Style.RESET_ALL}")
        pong_data = args[0] if args else struct.pack('!Q', time.time_ns())
        print(f"""
{Fore.BLUE}PONG Frame Details:{Style.RESET_ALL}
    FIN: True
    RSV1: False
    RSV2: False
    RSV3: False
    Opcode: PONG (0x0A)
    Masked: False
    Payload length: {len(pong_data)} bytes
    Payload (hex): {pong_data.hex()}
    Raw frame bytes: 0x8A {len(pong_data):02x} {pong_data.hex()}
        """)
        return await original_pong(*args, **kwargs)

    # Override the default ping handler
    async def custom_ping_handler(ping_frame):
        print(f"\n{Fore.CYAN}Received PING from client{Style.RESET_ALL}")
        print(f"""
{Fore.BLUE}Received PING Frame Details:{Style.RESET_ALL}
    FIN: {ping_frame.fin}
    RSV1: {ping_frame.rsv1}
    RSV2: {ping_frame.rsv2}
    RSV3: {ping_frame.rsv3}
    Opcode: PING (0x09)
    Masked: {ping_frame.masked}
    Payload length: {len(ping_frame.data) if hasattr(ping_frame, 'data') else 0} bytes
    Payload (hex): {ping_frame.data.hex() if hasattr(ping_frame, 'data') and ping_frame.data else 'None'}
    Mask key: {ping_frame.mask if hasattr(ping_frame, 'mask') else 'None'}
        """)
        return None

    # Override the default pong handler to log received pong frames
    async def custom_pong_handler(pong_frame):
        print(f"\n{Fore.CYAN}Received PONG from client{Style.RESET_ALL}")
        print(f"""
{Fore.BLUE}Received PONG Frame Details:{Style.RESET_ALL}
    FIN: {pong_frame.fin}
    RSV1: {pong_frame.rsv1}
    RSV2: {pong_frame.rsv2}
    RSV3: {pong_frame.rsv3}
    Opcode: PONG (0x0A)
    Masked: {pong_frame.masked}
    Payload length: {len(pong_frame.data) if hasattr(pong_frame, 'data') else 0} bytes
    Payload (hex): {pong_frame.data.hex() if hasattr(pong_frame, 'data') and pong_frame.data else 'None'}
    Mask key: {pong_frame.mask if hasattr(pong_frame, 'mask') else 'None'}
        """)
        return None

    websocket.ping = ping_wrapper
    websocket.pong = pong_wrapper
    websocket.ping_handler = custom_ping_handler
    websocket.pong_handler = custom_pong_handler
    
    # Set shorter ping interval for testing
    websocket.ping_interval = None  # Disable automatic pings
    websocket.ping_timeout = None   # Disable ping timeout
    
    try:
        while not break_loop:
            # Receive a message from the client
            print(f"\n{Fore.YELLOW}Sleeping for 25 seconds{Style.RESET_ALL}")
            time.sleep(2)
            message = await websocket.recv()
            if message is None:
                break

            # Get the frame information from the websocket
            if hasattr(websocket, 'frame'):
                print(f"\n{Fore.GREEN}Received Message Frame:{Style.RESET_ALL}")
                print(format_frame_info(websocket.frame))
            
            if isinstance(message, bytes):
                print(f"Received message content: {Fore.GREEN}{message[:100].hex()}{'...' if len(message) > 100 else ''}{Style.RESET_ALL}")
            else:
                print(f"Received message content: {Fore.GREEN}{message[:100]}{'...' if len(message) > 100 else ''}{Style.RESET_ALL}")
            
            # Send response
            await websocket.send(message)
            if isinstance(message, bytes):
                print(f"Sent: {Fore.YELLOW}{message[:100].hex()}{'...' if len(message) > 100 else ''}{Style.RESET_ALL}")
            else:
                print(f"Sent: {Fore.YELLOW}{message[:100]}{'...' if len(message) > 100 else ''}{Style.RESET_ALL}")
            
            # Send an additional message to the client
            # additional_message = "Server received your message!"
            # await websocket.send(additional_message)
            # print(f"Sent additional message: {Fore.YELLOW}{additional_message}{Style.RESET_ALL}")
            
            # Receive another message from the client
            second_message = await websocket.recv()
            if second_message is None:
                break
            print(f"Received second message: {Fore.GREEN}{second_message}{Style.RESET_ALL}")
            
            # Send a large data response
            large_data = "A" * 905000  # 1 million 'A' characters
            await websocket.send(large_data)
            print(f"Sent large data response: {Fore.YELLOW}[1 million 'A' characters]{Style.RESET_ALL}")
            
            # Send a final response
            # final_response = "Thank you for your messages. Goodbye!"
            # await websocket.send(final_response)
            # print(f"Sent final response: {Fore.YELLOW}{final_response}{Style.RESET_ALL}")
            
    except websockets.exceptions.ConnectionClosedError as e:
        # If the connection is closed unexpectedly, this exception is caught
        # We simply pass, effectively closing the connection gracefully
        print(f"\nConnection closed reason: CLIENT DISCONNECTED: {e}\n\n")

def format_frame_info(frame):
    """Helper function to format WebSocket frame details"""
    if hasattr(frame, 'opcode'):
        opcodes = {
            0x0: "CONTINUATION",
            0x1: "TEXT",
            0x2: "BINARY",
            0x8: "CLOSE",
            0x9: "PING",
            0xA: "PONG"
        }
        opcode_str = opcodes.get(frame.opcode, f"UNKNOWN({frame.opcode})")
        
        frame_info = f"""
{Fore.BLUE}Frame Details:{Style.RESET_ALL}
    FIN: {frame.fin}
    RSV1: {frame.rsv1}
    RSV2: {frame.rsv2}
    RSV3: {frame.rsv3}
    Opcode: {opcode_str} (0x{frame.opcode:02x})
    Masked: {frame.masked}
    Payload length: {len(frame.data) if hasattr(frame, 'data') else 0} bytes
    """
        if hasattr(frame, 'data') and frame.data:
            frame_info += f"    Payload (hex): {frame.data.hex()[:50]}{'...' if len(frame.data) > 25 else ''}"
        return frame_info
    return "Frame information not available"

# Define an asynchronous function named 'main' that sets up the server
async def main():
    global break_loop
    # Create a WebSocket server using the 'serve' function from the websockets library
    # It will use the 'echo' function to handle connections, listen on 'localhost' at port 8765
    server = await websockets.serve(echo, "localhost", 8765)
    print("Server started. Press Ctrl+C to stop.")
    try:
        # Wait for the server to close
        await server.wait_closed()
    except asyncio.CancelledError:
        print("Server is shutting down...")
        break_loop = True
    finally:
        server.close()
        await server.wait_closed()
        print("Server has been shut down.")

# Run the 'main' function using the 'run' function from the asyncio library
# This is a blocking call that runs the 'main' function and waits for it to complete
if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("Keyboard interrupt received. Shutting down...")
