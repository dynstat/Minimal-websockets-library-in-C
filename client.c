#include <libwebsockets.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <winsock2.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "crypt32.lib")


#define ENCRYPTION_KEY "346hklase89345hklhdsjhgrd7t34fet"
#define MAX_PAYLOAD 4096
// #define LWS_SEND_BUFFER_PRE_PADDING 16
// #define LWS_SEND_BUFFER_POST_PADDING 16

// ... (we'll add function declarations and global variables here)

void handle_openssl_error(void) {
    ERR_print_errors_fp(stderr);
    exit(1);
}

void encrypt_message(const unsigned char *message, size_t message_len, unsigned char **encrypted, size_t *encrypted_len) {
    EVP_CIPHER_CTX *ctx;
    int len;
    int ciphertext_len;
    unsigned char key[32];
    unsigned char iv[16];

    *encrypted = NULL;
    *encrypted_len = 0;

    // Generate random IV
    if (RAND_bytes(iv, sizeof(iv)) != 1) {
        handle_openssl_error();
    }

    // Decode base64 key
    EVP_DecodeBlock(key, (unsigned char *)ENCRYPTION_KEY, strlen(ENCRYPTION_KEY));

    // Create and initialize the context
    if (!(ctx = EVP_CIPHER_CTX_new())) handle_openssl_error();

    // Initialize the encryption operation
    if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv))
        handle_openssl_error();

    // Allocate memory for the encrypted message (message length + block size for padding + IV)
    *encrypted = malloc(message_len + EVP_CIPHER_block_size(EVP_aes_256_cbc()) + 16);
    if (!*encrypted) {
        fprintf(stderr, "Failed to allocate memory\n");
        exit(1);
    }

    // Copy IV to the beginning of the encrypted message
    memcpy(*encrypted, iv, 16);
    ciphertext_len = 16;

    // Encrypt the message
    if (1 != EVP_EncryptUpdate(ctx, *encrypted + 16, &len, message, message_len))
        handle_openssl_error();
    ciphertext_len += len;

    // Finalize the encryption
    if (1 != EVP_EncryptFinal_ex(ctx, *encrypted + ciphertext_len, &len))
        handle_openssl_error();
    ciphertext_len += len;

    *encrypted_len = ciphertext_len;

    // Clean up
    EVP_CIPHER_CTX_free(ctx);
}

void decrypt_message(const unsigned char *encrypted, size_t encrypted_len, unsigned char **decrypted, size_t *decrypted_len) {
    EVP_CIPHER_CTX *ctx;
    int len;
    int plaintext_len;
    unsigned char key[32];
    unsigned char iv[16];

    *decrypted = NULL;
    *decrypted_len = 0;

    // Extract IV from the beginning of the encrypted message
    memcpy(iv, encrypted, 16);

    // Decode base64 key
    EVP_DecodeBlock(key, (unsigned char *)ENCRYPTION_KEY, strlen(ENCRYPTION_KEY));

    // Create and initialize the context
    if (!(ctx = EVP_CIPHER_CTX_new())) handle_openssl_error();

    // Initialize the decryption operation
    if (1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv))
        handle_openssl_error();

    // Allocate memory for the decrypted message
    *decrypted = malloc(encrypted_len);
    if (!*decrypted) {
        fprintf(stderr, "Failed to allocate memory\n");
        exit(1);
    }

    // Decrypt the message
    if (1 != EVP_DecryptUpdate(ctx, *decrypted, &len, encrypted + 16, encrypted_len - 16))
        handle_openssl_error();
    plaintext_len = len;

    // Finalize the decryption
    if (1 != EVP_DecryptFinal_ex(ctx, *decrypted + len, &len))
        handle_openssl_error();
    plaintext_len += len;

    *decrypted_len = plaintext_len;

    // Clean up
    EVP_CIPHER_CTX_free(ctx);
}

// ... (we'll add more functions here)

static int callback_client(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("Connection established\n");
            // Send authentication token
            // ... (implement send_encrypted_message here)
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            // Handle received messages
            // ... (implement receive_encrypted_message here)
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            printf("Connection closed\n");
            break;

        default:
            break;
    }

    return 0;
}

int main(void) {

    printf("Program started\n");
    struct lws_context_creation_info info;
    struct lws_client_connect_info conn_info;
    struct lws_context *context;
    const char *address = "localhost";
    int port = 8765;

    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = (struct lws_protocols[]) {
        {
            "client-protocol",
            callback_client,
            0,
            MAX_PAYLOAD,
        },
        { NULL, NULL, 0, 0 }
    };
    info.gid = -1;
    info.uid = -1;

    context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "Failed to create LWS context\n");
        return 1;
    }

    memset(&conn_info, 0, sizeof(conn_info));
    conn_info.context = context;
    conn_info.address = address;
    conn_info.port = port;
    conn_info.path = "/";
    conn_info.host = lws_canonical_hostname(context);
    conn_info.origin = "origin";
    conn_info.protocol = "client-protocol";

    lws_client_connect_via_info(&conn_info);

    while (1) {
        lws_service(context, 50);
    }

    lws_context_destroy(context);

    return 0;
}

