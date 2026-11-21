#ifndef QR_H
#define QR_H

#include <stdint.h>
#include <stddef.h>

// Generate and print a QR code to the terminal using ANSI half-block characters.
// data: null-terminated string to encode (e.g., "WIFI:T:WPA;S:MySSID;P:MyPass;;")
// Returns 0 on success, -1 on error (data too long, etc.)
int qr_print_terminal(const char *data);

// Generate QR code into a matrix buffer.
// data: null-terminated string to encode
// matrix: output buffer, caller-allocated, at least qr_matrix_size()*qr_matrix_size() bytes
// Each byte is 1 (dark) or 0 (light)
// Returns the side length of the QR matrix on success, -1 on error
int qr_generate(const char *data, uint8_t *matrix, size_t matrix_buf_size);

// Returns the maximum possible side length for the supported QR versions
int qr_max_size(void);

#endif

