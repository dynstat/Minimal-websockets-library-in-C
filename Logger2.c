#include "Logger2.h"

// Initialize the file path variable globally to avoid repetition in each function
// const char *filePath2 = "C:\\ProgramData\\mws_lib.log";
const char *filePath2 = "C:\\ProgramData\\softTokenReaderDriver.log";

void logToFile2(const char *str)
{
    // UNREFERENCED_PARAMETER(str);
    FILE *file;
    errno_t err;
    const char *mode = "a"; // Specify the file mode

    // Open the file using fopen_s
    err = fopen_s(&file, filePath2, mode);

    // Check if the file was opened successfully
    if (err == 0 && file != NULL)
    {
        // Write the string to the file
        fprintf(file, "%s", str);
        // Close the file
        fclose(file);
    }
}

void logToFileI2(long some_int)
{
    // UNREFERENCED_PARAMETER(some_int);
    FILE *file;
    errno_t err;
    const char *mode = "a"; // Specify the file mode

    // Open the file using fopen_s
    err = fopen_s(&file, filePath2, mode);

    // Check if the file was opened successfully
    if (err == 0 && file != NULL)
    {
        // Write the unsigned long integer to the file
        fprintf(file, "%ld\n", some_int);
        // Close the file
        fclose(file);
    }
}

void printHexBytesString2(const char *input, size_t size)
{
    char hexBuffer[2]; // Buffer to hold each formatted hex byte (2 digits + space)
    size_t index = 0;

    for (size_t i = 0; i < size; i++)
    {
        snprintf(hexBuffer, sizeof(hexBuffer), "%02X ", input[i]);
        logToFile2(hexBuffer);
        index += sizeof(hexBuffer) - 1; // Adjust the index based on the number of characters written
    }
    logToFile2("\n");
}

// #################################################################

void printByteArrayToFile2(const char *byteArray, size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        // Convert each byte to a string and log it to the file
        char byteStr[4]; // Assuming 2 hex digits and 1 newline character
        snprintf(byteStr, sizeof(byteStr), "%02x ", (unsigned char)byteArray[i]);

        logToFile2(byteStr);
    }
    logToFile2("\nSize of buffer is: ");
    logToFileI2((long)size);
}

void print_state2(int state)
{

    switch (state)
    {
    case 0:
        logToFile2("\nstate is SCARD_UNKNOWN");
        break;
    case 1:
        logToFile2("\nstate is SCARD_ABSENT");
        break;
    case 2:
        logToFile2("\nstate is SCARD_PRESENT");
        break;
    case 3:
        logToFile2("\nstate is SCARD_SWALLOWED");
        break;
    case 4:
        logToFile2("\nstate is SCARD_POWERED");
        break;
    case 5:
        logToFile2("\nstate is SCARD_NEGOTIABLE");
        break;
    case 6:
        logToFile2("\nstate is SCARD_SPECIFIC");
        break;

    default:
        break;
    }
}