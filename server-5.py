import asyncio
import websockets
from colorama import Fore, Style, init
import struct
import time
import platform
import ctypes
import os
from pathlib import Path
import traceback
from contextlib import contextmanager

# Initialize colorama
init(autoreset=True)

break_loop = False

# Global variables
start_time = time.time()
softoken = None
server = None
is_initialized = False

def get_library_path():
    system = platform.system()
    print(f"System: {system} : current path: {os.path.abspath(os.path.dirname(__file__))}")
    if system == "Windows":
        return r".\libsoftoken.dll"
    elif system == "Darwin":  # macOS
        return str(Path.home() / "Library" / "Application Support" / "softoken" / "libsoftoken.dylib")
    elif system == "Linux":
        return "/usr/lib/softoken/libsoftoken.so"
    else:
        raise OSError(f"Unsupported operating system: {system}")

def load_library():
    print("Loading library")
    global softoken
    
    lib_path = get_library_path()
    print(f"***************** Library path: {lib_path} *******************")
    if not os.path.exists(lib_path):
        print(f"path = {lib_path}")
        raise FileNotFoundError(f"Library file not found: lib_path checked: {lib_path} system: {platform.system()}")

    try:
        handle = ctypes.CDLL(lib_path)
        handle.init_softToken.argtypes = []
        handle.init_softToken.restype = ctypes.c_ubyte

        handle.SendApdu_softToken.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.POINTER(ctypes.c_int),
        ]
        handle.SendApdu_softToken.restype = None
        return handle

    except OSError as e:
        print(f"Failed to load Softoken library: {e}")
        raise

def unload_library(lib):
    global softoken, logger
    print("Unloading library")
    system = platform.system()
    try:
        if system == "Windows":
            handle = lib._handle
            print(f"Library handle value: {handle}")
            handle_p = ctypes.c_void_p(handle)

            result = ctypes.windll.kernel32.FreeLibrary(handle_p)
            if result == 0:
                error = ctypes.get_last_error()
                raise OSError(f"Failed to unload library: error code {error}")
        elif system in ["Darwin", "Linux"]:
            libc = ctypes.CDLL(None)
            if not hasattr(libc, "dlclose"):
                raise AttributeError("dlclose not found in libc")

            libc.dlclose.argtypes = [ctypes.c_void_p]
            libc.dlclose.restype = ctypes.c_int

            dlerror = libc.dlerror
            dlerror.argtypes = []
            dlerror.restype = ctypes.c_char_p

            dlerror()

            result = libc.dlclose(lib._handle)
            if result != 0:
                error = dlerror()
                error_msg = error.decode("utf-8") if error else "Unknown error"
                raise OSError(f"Failed to unload library: {error_msg}")
        else:
            raise OSError(f"Unsupported operating system: {system}")

        print("Softoken library unloaded successfully")
        softoken = None
    except Exception as e:
        print(f"Error unloading library: {e}")
        print(f"Library handle type: {type(lib._handle)}")
        print(f"Library handle value: {lib._handle}")
        print(traceback.format_exc())
    finally:
        softoken = None

def handle_apdu(apdu_command):
    global softoken, logger
    command_len = len(apdu_command)
    response_len = ctypes.c_int(10000)
    response_apdu = (ctypes.c_ubyte * 10000)()

    try:
        softoken.SendApdu_softToken(
            (ctypes.c_ubyte * command_len)(*apdu_command),
            command_len,
            response_apdu,
            ctypes.byref(response_len),
        )

        return bytes(response_apdu[: response_len.value])
    except Exception as e:
        print(f"APDU processing error: {e}")
        raise

async def handle_client(websocket: websockets.WebSocketServerProtocol):
    global break_loop, softoken
    # Set shorter ping interval for testing
    websocket.ping_interval = None  # Disable automatic pings
    websocket.ping_timeout = None   # Disable ping timeout
    
    print(f"New connection from {websocket.remote_address}")
    
    try:
        while not break_loop:
            message = await websocket.recv()
            if message is None:
                break

            if isinstance(message, bytes):
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
                    elapsed_time = time.time() - start_time
                    tag_sent = 90
                    if elapsed_time <= 5:
                        resp = b"\x6d\x82"
                        print("First 5 seconds: Returning 6D82")
                    else:
                        apdu_command = message[2::]  # Extract command portion
                        try:
                            resp = handle_apdu(apdu_command)
                            print(f"APDU command processed successfully, response length: {len(resp)}")
                        except Exception as e:
                            print(f"Error processing APDU command: {e}")
                            resp = b"\x6d\x82"  # Return error in case of failure
                        
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
    global break_loop, softoken, is_initialized
    
    # Load and initialize library once at startup
    try:
        if not is_initialized:
            softoken = load_library()
            result = softoken.init_softToken()
            if result != 0:
                raise RuntimeError(f"Failed to initialize softoken, error code: {result}")
            is_initialized = True
            print("Library loaded and initialized successfully")
    except Exception as e:
        print(f"Error loading/initializing library: {e}")
        return

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
        if softoken:
            try:
                unload_library(softoken)
                print("Library unloaded successfully")
            except Exception as e:
                print(f"Error unloading library: {e}")
        server.close()
        await server.wait_closed()
        print("Server has been shut down.")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nKeyboard interrupt received. Shutting down...")