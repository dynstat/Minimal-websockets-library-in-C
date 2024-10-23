import asyncio
import websockets
import json
import logging
import os
import traceback
from datetime import datetime




import hashlib
import uuid
from datetime import datetime
import platform
import subprocess
import re


# cython: language_level=3
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import padding
import os

ENCRYPTION_KEY = b"346hklase89345hklhdsjhgrd7t34fet87345iuhdsf78"[:32]

def encrypt_message(message):
    """Encrypt a message using AES-256 with an IV."""
    iv = os.urandom(16)  # Generate a random 16-byte IV
    cipher = Cipher(algorithms.AES(ENCRYPTION_KEY), modes.CBC(iv), backend=default_backend())
    encryptor = cipher.encryptor()
    
    # Pad the message to be a multiple of the block size (16 bytes for AES)
    padder = padding.PKCS7(algorithms.AES.block_size).padder()
    padded_message = padder.update(message.encode()) + padder.finalize()
    
    encrypted_message = encryptor.update(padded_message) + encryptor.finalize()
    return iv + encrypted_message  # Prepend IV to the encrypted message

def decrypt_message(encrypted_message):
    """Decrypt a message using AES-256 with an IV."""
    iv = encrypted_message[:16]  # Extract the IV from the beginning
    actual_encrypted_message = encrypted_message[16:]
    
    cipher = Cipher(algorithms.AES(ENCRYPTION_KEY), modes.CBC(iv), backend=default_backend())
    decryptor = cipher.decryptor()
    
    padded_message = decryptor.update(actual_encrypted_message) + decryptor.finalize()
    
    # Unpad the message
    unpadder = padding.PKCS7(algorithms.AES.block_size).unpadder()
    message = unpadder.update(padded_message) + unpadder.finalize()
    
    return message.decode()

def authenticate(token):
    """Authenticate a token."""
    # Implement your authentication logic here
    return token == "valid_token"  # Placeholder

def is_localhost(ip_address):
    """Check if the given IP address is localhost."""
    return ip_address == "127.0.0.1"


def get_mac_address():
    """
    Get the MAC address of the machine in a platform-independent way.
    
    :return: MAC address as a string of 12 lowercase hexadecimal digits
    """
    if platform.system() == "Windows":
        # Windows-specific method
        output = subprocess.check_output("getmac").decode()
        mac = re.search(r"([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})", output).group()
    elif platform.system() == "Darwin" or platform.system() == "Linux":
        # macOS and Linux method
        mac = ':'.join(['{:02x}'.format((uuid.getnode() >> elements) & 0xff) 
                        for elements in range(0, 8*6, 8)][::-1])
    else:
        raise OSError("Unsupported operating system")
    
    # Remove any colons or hyphens and ensure lowercase
    return ''.join(c.lower() for c in mac if c not in ':-')

def generate_client_name(port):
    """
    Generate a client name based on MAC address, current date, and port number.
    
    :param port: The port number of the client connection
    :return: A tuple containing (client_name, mac_xor_date)
    """
    mac_bytes = bytes.fromhex(get_mac_address())
    current_date = datetime.now().strftime("%d%H%y")
    date_bytes = current_date.encode()
    
    # Ensure mac_bytes and date_bytes have the same length
    min_length = min(len(mac_bytes), len(date_bytes))
    
    
    # Perform XOR operation between MAC address and date bytes, then convert to hexadecimal
    # Explanation:
    # 1. zip(mac_bytes[:min_length], date_bytes[:min_length]) pairs up corresponding bytes
    # 2. a ^ b performs XOR operation between each pair of bytes
    # 3. bytes(...) creates a new bytes object from the XOR results
    # 4. .hex() converts the bytes to a hexadecimal string
    
    # Example:
    # If mac_bytes = b'\x12\x34\x56' and date_bytes = b'\xab\xcd\xef'
    # The XOR operation would be:
    #   0x12 ^ 0xab = 0xb9
    #   0x34 ^ 0xcd = 0xf9
    #   0x56 ^ 0xef = 0xb9
    # Resulting in: b'\xb9\xf9\xb9'
    # After .hex(), the final result would be: 'b9f9b9'
    mac_xor_date = bytes(a ^ b for a, b in zip(mac_bytes[:min_length], date_bytes[:min_length])).hex()
    
    client_name = encode_client_name(mac_xor_date, port)
    
    return client_name, mac_xor_date

def encode_client_name(mac_xor_date, port):
    """
    Encode the client name using MAC XOR date and port number.
    
    :param mac_xor_date: The result of XORing MAC address with current date
    :param port: The port number of the client connection
    :return: Encoded client name
    """
    combined = f"{mac_xor_date}{port}"
    hash_object = hashlib.sha256(combined.encode())
    return hash_object.hexdigest()[:12]

def create_client_name_request():
    """
    Create a client name request message.
    
    :return: Bytes object containing the request message
    """
    return b'\x81' + len("clname").to_bytes(2, 'big') + b"clname"

def create_client_name_response(client_name):
    """
    Create a client name response message.
    
    :param client_name: The generated client name
    :return: Bytes object containing the response message
    """
    return b'\x82' + len(client_name).to_bytes(2, 'big') + client_name.encode()

def parse_client_name_response(response):
    """
    Parse the client name response message.
    
    :param response: The received response message
    :return: The decoded client name
    """
    if response[0] != 0x82:
        raise ValueError("Invalid client response tag")
    
    response_length = int.from_bytes(response[1:3], 'big')
    return response[3:3+response_length].decode()

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger('softoken')

# Global variables
current_client = None
HEARTBEAT_INTERVAL = 30
HEARTBEAT_TIMEOUT = 10

async def send_encrypted_message(websocket, tag, data):
    message = json.dumps(data).encode()
    encrypted_message = encrypt_message(message)
    full_message = bytes([tag]) + len(encrypted_message).to_bytes(2, 'big') + encrypted_message
    await websocket.send(full_message)

async def receive_encrypted_message(websocket):
    response = await websocket.recv()
    tag = response[0]
    length = int.from_bytes(response[1:3], 'big')
    encrypted_data = response[3:3+length]
    decrypted_data = decrypt_message(encrypted_data)
    data = json.loads(decrypted_data)
    return tag, data



async def handle_connection(websocket, path):
    global current_client
    client_info = None
    heartbeat_task = None

    async def heartbeat():
        try:
            while True:
                await asyncio.sleep(HEARTBEAT_INTERVAL)
                try:
                    await asyncio.wait_for(websocket.ping(), timeout=HEARTBEAT_TIMEOUT)
                except asyncio.TimeoutError:
                    raise websockets.exceptions.ConnectionClosed(1011, "Client heartbeat timeout")
        except asyncio.CancelledError:
            pass
    
    try:
        logger.info("New connection received")
        
        if not is_localhost(websocket.remote_address[0]):
            logger.warning("Non-localhost connection attempt")
            await websocket.close(code=4003, reason="Only localhost connections are allowed")
            return

        if current_client:
            logger.warning("Connection attempt while another client is connected")
            await websocket.close(code=4002, reason="Server is busy with another client")
            return

        logger.info("Waiting for authentication token")
        tag, auth_data = await receive_encrypted_message(websocket)
        logger.info(f"Received message with tag: {tag}")
        if tag != 0x79 or not authenticate(auth_data.get('token')):
            logger.warning("Authentication failed")
            await websocket.close(code=4001, reason="Authentication failed")
            return

        logger.info("Authentication successful")

        await send_encrypted_message(websocket, 0x80, {"request": "client_name"})
        logger.info("Sent client name request")

        tag, client_data = await receive_encrypted_message(websocket)
        if tag != 0x81:
            raise ValueError("Invalid client name response tag")
        received_client_name = client_data.get("client_name")

        port = websocket.remote_address[1]
        expected_name, _ = generate_client_name(port)

        if received_client_name != expected_name:
            logger.warning(f"Client name verification failed. Received: {received_client_name}")
            await send_encrypted_message(websocket, 0x82, {"status": "authfail"})
            await websocket.close(code=4004, reason="Client name verification failed")
            return

        await send_encrypted_message(websocket, 0x82, {"status": "authok"})
        logger.info(f"Client authenticated successfully: {received_client_name}")

        client_info = f"{websocket.remote_address[0]}:{port}"
        current_client = client_info
        logger.info(f"Client connected and verified: {client_info} and id: {received_client_name}")

        heartbeat_task = asyncio.create_task(heartbeat())

        while True:
            tag, data = await receive_encrypted_message(websocket)
            
            if tag == 0x83:
                logger.info(f"Received initialization request: {data}")
                # Here you would initialize your softoken
                # For now, we'll just send a success response
                await send_encrypted_message(websocket, 0x84, {"status": "success"})
                logger.info("Sent initialization success response")
            elif tag == 0x85:
                try:
                    apdu_command = bytes(data['data'])
                    logger.info(f"Received APDU command: {apdu_command.hex()}")
                    
                    # Here you would handle the APDU command
                    # For now, we'll just send a dummy response
                    response_apdu = b'\x90\x00'
                    
                    logger.info(f"Sending APDU response: {response_apdu.hex()}")
                    await send_encrypted_message(websocket, 0x86, {"status": "success", "response": list(response_apdu)})
                except Exception as e:
                    logger.error(f"Error processing APDU command: {e}")
                    await send_encrypted_message(websocket, 0x86, {"status": "failure", "response": []})
            else:
                logger.warning(f"Unexpected message tag: {tag}")

    except websockets.exceptions.ConnectionClosed as e:
        logger.info(f"Client disconnected: {client_info}. Reason: {e.reason}")
    except Exception as e:
        logger.error(f"Error in WebSocket connection: {e}")
        logger.error(traceback.format_exc())
    finally:
        if heartbeat_task:
            heartbeat_task.cancel()
        current_client = None
        if client_info:
            logger.info(f"Connection closed for client: {client_info} and id: {received_client_name}")

async def main():
    server = await websockets.serve(
        handle_connection,
        "127.0.0.1",
        8765,
        max_size=1048576,
        max_queue=1,
        compression=None,
        ping_interval=None,
        ping_timeout=None,
    )
    logger.info("Server started on 127.0.0.1:8765")
    await server.wait_closed()

if __name__ == "__main__":
    asyncio.run(main())