# Import the asyncio library, which provides infrastructure for writing asynchronous code
import asyncio
# Import the websockets library, which implements the WebSocket protocol
import websockets

# Import colorama for colored terminal output
from colorama import Fore, Style, init

# Initialize colorama
init(autoreset=True)

# Define an asynchronous function named 'echo' that handles WebSocket connections
# The 'async' keyword indicates that this function can be paused and resumed
async def echo(websocket: websockets.WebSocketServerProtocol):
    print(f"New connection: with address {websocket.remote_address} and port {websocket.port}")
    try:
        while True:
            # Receive a message from the client
            message = await websocket.recv()
            if message is None:
                break

            # Print the received message to the console in green
            print(f"Received: {Fore.GREEN}{message}{Style.RESET_ALL}")
            # Send the same message back to the client
            # The 'await' keyword is used to pause execution until the send operation is complete
            await websocket.send(message)
            print(f"Sent: {Fore.YELLOW}{message}{Style.RESET_ALL}")
            
            # Send an additional message to the client
            additional_message = "Server received your message!"
            await websocket.send(additional_message)
            print(f"Sent additional message: {Fore.YELLOW}{additional_message}{Style.RESET_ALL}")
            
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

# Define an asynchronous function named 'main' that sets up the server
async def main():
    # Create a WebSocket server using the 'serve' function from the websockets library
    # It will use the 'echo' function to handle connections, listen on 'localhost' at port 8765
    server = await websockets.serve(echo, "localhost", 8765)
    print("Server started. Press Ctrl+C to stop.")
    try:
        # Wait for the server to close
        await server.wait_closed()
    except asyncio.CancelledError:
        print("Server is shutting down...")
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
