#ifndef LOGGER2_H
#define LOGGER2_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Any other Windows headers
#endif

#include <stdio.h>
void logToFile2(const char *);
void logToFileI2(long);
void print_state2(int state);
void printByteArrayToFile2(const char *byteArray, size_t size);
void printHexBytesString2(const char* input, size_t size);

#endif // LOGGER2_H
