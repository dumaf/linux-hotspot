#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "qr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define QR_MIN_VERSION 1
#define QR_MAX_VERSION 10
#define QR_MAX_SIDE    57  // 17 + 4 * 10

// Version specification structure for Error Correction Level M
typedef struct {
    int version;
    int size;                   // Side length: 17 + 4 * version
    int total_codewords;        // Total data + EC codewords
    int data_codewords;         // Number of data codewords
    int ec_per_block;           // Number of EC codewords per block
    int group1_blocks;          // Blocks in Group 1
    int group1_data_per_block;  // Data codewords per block in Group 1
    int group2_blocks;          // Blocks in Group 2
    int group2_data_per_block;  // Data codewords per block in Group 2
    int align_count;            // Number of alignment coordinates
    int align_coords[7];        // Alignment pattern center coordinates
} qr_version_info_t;

// Standard table for QR Code Model 2, Versions 1-10, EC Level M
static const qr_version_info_t QR_VERSIONS[QR_MAX_VERSION + 1] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}},
    // V1: 21x21, 26 total, 16 data, 10 EC (1 block of 16 data)
    {1, 21, 26, 16, 10, 1, 16, 0, 0, 0, {0}},
    // V2: 25x25, 44 total, 28 data, 16 EC (1 block of 28 data)
    {2, 25, 44, 28, 16, 1, 28, 0, 0, 2, {6, 18}},
    // V3: 29x29, 70 total, 44 data, 26 EC (1 block of 44 data)
    {3, 29, 70, 44, 26, 1, 44, 0, 0, 2, {6, 22}},
    // V4: 33x33, 100 total, 64 data, 18 EC (2 blocks of 32 data)
    {4, 33, 100, 64, 18, 2, 32, 0, 0, 2, {6, 26}},
    // V5: 37x37, 134 total, 86 data, 24 EC (2 blocks of 43 data)
    {5, 37, 134, 86, 24, 2, 43, 0, 0, 2, {6, 30}},
    // V6: 41x41, 172 total, 108 data, 16 EC (4 blocks of 27 data)
    {6, 41, 172, 108, 16, 4, 27, 0, 0, 2, {6, 34}},
    // V7: 45x45, 196 total, 124 data, 18 EC (4 blocks of 31 data)
    {7, 45, 196, 124, 18, 4, 31, 0, 0, 3, {6, 22, 38}},
    // V8: 49x49, 242 total, 154 data, 22 EC (2 blocks of 38, 2 blocks of 39)
    {8, 49, 242, 154, 22, 2, 38, 2, 39, 3, {6, 24, 42}},
    // V9: 53x53, 292 total, 182 data, 22 EC (3 blocks of 36, 2 blocks of 37)
    {9, 53, 292, 182, 22, 3, 36, 2, 37, 3, {6, 26, 46}},
    // V10: 57x57, 346 total, 216 data, 26 EC (4 blocks of 43, 1 block of 44)
    {10, 57, 346, 216, 26, 4, 43, 1, 44, 3, {6, 28, 50}}
};

// Returns maximum side length for supported versions (V10 = 57)
int qr_max_size(void) {
    return QR_MAX_SIDE;
}

/* ========================================================================= */
/* Galois Field GF(2^8) and Reed-Solomon Error Correction                    */
/* Primitive polynomial: x^8 + x^4 + x^3 + x^2 + 1 (0x11D = 285)             */
/* ========================================================================= */

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static bool gf_initialized = false;

static void gf_init(void) {
    if (gf_initialized) {
        return;
    }
    int val = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)val;
        gf_exp[i + 255] = (uint8_t)val;
        gf_log[val] = (uint8_t)i;
        val <<= 1;
        if (val & 0x100) {
            val ^= 0x11D;
        }
    }
    gf_log[0] = 0; // Undefined mathematically, set to 0
    gf_initialized = true;
}

static inline uint8_t gf_mul(uint8_t x, uint8_t y) {
    if (x == 0 || y == 0) {
        return 0;
    }
    return gf_exp[gf_log[x] + gf_log[y]];
}

// Compute Reed-Solomon generator polynomial of degree `ec_count`
// gen array must have at least ec_count + 1 elements
static void rs_generator_poly(int ec_count, uint8_t *gen) {
    memset(gen, 0, (size_t)(ec_count + 1));
    gen[0] = 1;
    for (int i = 0; i < ec_count; i++) {
        uint8_t root = gf_exp[i];
        for (int j = i + 1; j >= 1; j--) {
            gen[j] = gen[j] ^ gf_mul(gen[j - 1], root);
        }
    }
}

// Compute Reed-Solomon error correction codewords for a single data block
static void rs_compute_ec(const uint8_t *data, int data_len, int ec_count,
                          const uint8_t *gen, uint8_t *ec_out) {
    memset(ec_out, 0, (size_t)ec_count);
    for (int i = 0; i < data_len; i++) {
        uint8_t factor = data[i] ^ ec_out[0];
        memmove(&ec_out[0], &ec_out[1], (size_t)(ec_count - 1));
        ec_out[ec_count - 1] = 0;
        for (int j = 0; j < ec_count; j++) {
            ec_out[j] ^= gf_mul(gen[j + 1], factor);
        }
    }
}

/* ========================================================================= */
/* Bit Stream Helper                                                         */
/* ========================================================================= */

typedef struct {
    uint8_t *buf;
    size_t bit_len;
    size_t capacity_bits;
} bit_stream_t;

static void bs_init(bit_stream_t *bs, uint8_t *buf, size_t capacity_bytes) {
    bs->buf = buf;
    bs->bit_len = 0;
    bs->capacity_bits = capacity_bytes * 8;
    memset(buf, 0, capacity_bytes);
}

static void bs_append_bits(bit_stream_t *bs, uint32_t val, int count) {
    for (int i = count - 1; i >= 0; i--) {
        if (bs->bit_len < bs->capacity_bits) {
            int bit = (val >> i) & 1;
            if (bit) {
                bs->buf[bs->bit_len / 8] |= (uint8_t)(1 << (7 - (bs->bit_len % 8)));
            }
            bs->bit_len++;
        }
    }
}

/* ========================================================================= */
/* QR Matrix Structure and Function Pattern Placement                        */
/* ========================================================================= */

typedef struct {
    int version;
    int size;
    uint8_t modules[QR_MAX_SIDE][QR_MAX_SIDE];   // 0 = light, 1 = dark
    bool is_func[QR_MAX_SIDE][QR_MAX_SIDE];      // true if reserved/function module
} qr_matrix_t;

static void set_module(qr_matrix_t *qr, int r, int c, int val, bool is_function) {
    if (r >= 0 && r < qr->size && c >= 0 && c < qr->size) {
        qr->modules[r][c] = (uint8_t)(val ? 1 : 0);
        if (is_function) {
            qr->is_func[r][c] = true;
        }
    }
}

// Draw a 7x7 finder pattern and its 1-module quiet separator around it
static void place_finder_pattern(qr_matrix_t *qr, int top_r, int left_c) {
    for (int dr = -1; dr <= 7; dr++) {
        for (int dc = -1; dc <= 7; dc++) {
            int r = top_r + dr;
            int c = left_c + dc;
            if (r < 0 || r >= qr->size || c < 0 || c >= qr->size) {
                continue;
            }
            // Check if within 7x7 core
            if (dr >= 0 && dr <= 6 && dc >= 0 && dc <= 6) {
                if (dr == 0 || dr == 6 || dc == 0 || dc == 6 ||
                    (dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4)) {
                    set_module(qr, r, c, 1, true);
                } else {
                    set_module(qr, r, c, 0, true);
                }
            } else {
                // Separator ring
                set_module(qr, r, c, 0, true);
            }
        }
    }
}

// Place 5x5 alignment pattern centered at (center_r, center_c)
static void place_alignment_pattern(qr_matrix_t *qr, int center_r, int center_c) {
    for (int dr = -2; dr <= 2; dr++) {
        for (int dc = -2; dc <= 2; dc++) {
            int r = center_r + dr;
            int c = center_c + dc;
            if (abs(dr) == 2 || abs(dc) == 2 || (dr == 0 && dc == 0)) {
                set_module(qr, r, c, 1, true);
            } else {
                set_module(qr, r, c, 0, true);
            }
        }
    }
}

// Setup all fixed function patterns
static void setup_function_patterns(qr_matrix_t *qr) {
    const qr_version_info_t *vi = &QR_VERSIONS[qr->version];
    int size = qr->size;

    // 1. Finder patterns and separators
    place_finder_pattern(qr, 0, 0);                 // Top-Left
    place_finder_pattern(qr, 0, size - 7);          // Top-Right
    place_finder_pattern(qr, size - 7, 0);          // Bottom-Left

    // 2. Alignment patterns for Version >= 2
    for (int i = 0; i < vi->align_count; i++) {
        for (int j = 0; j < vi->align_count; j++) {
            int r = vi->align_coords[i];
            int c = vi->align_coords[j];
            // Skip alignment patterns that overlap with finder patterns (+ separators)
            bool overlap = false;
            for (int dr = -2; dr <= 2; dr++) {
                for (int dc = -2; dc <= 2; dc++) {
                    int nr = r + dr;
                    int nc = c + dc;
                    if ((nr < 9 && nc < 9) ||
                        (nr < 9 && nc >= size - 8) ||
                        (nr >= size - 8 && nc < 9)) {
                        overlap = true;
                        break;
                    }
                }
                if (overlap) break;
            }
            if (!overlap) {
                place_alignment_pattern(qr, r, c);
            }
        }
    }

    // 3. Timing patterns (Row 6 and Col 6)
    for (int i = 8; i < size - 8; i++) {
        if (!qr->is_func[6][i]) {
            set_module(qr, 6, i, (i % 2 == 0) ? 1 : 0, true);
        }
        if (!qr->is_func[i][6]) {
            set_module(qr, i, 6, (i % 2 == 0) ? 1 : 0, true);
        }
    }

    // 4. Dark module: always located at (size - 8, 8)
    set_module(qr, size - 8, 8, 1, true);

    // 5. Reserve format information areas
    // Around Top-Left finder
    for (int i = 0; i <= 8; i++) {
        qr->is_func[8][i] = true;
        qr->is_func[i][8] = true;
    }
    // Below Top-Right finder
    for (int i = 0; i < 8; i++) {
        qr->is_func[8][size - 1 - i] = true;
    }
    // To right of Bottom-Left finder
    for (int i = 0; i < 8; i++) {
        qr->is_func[size - 1 - i][8] = true;
    }

    // 6. Version information areas (Version >= 7)
    if (qr->version >= 7) {
        for (int i = 0; i < 6; i++) {
            for (int j = 0; j < 3; j++) {
                qr->is_func[i][size - 11 + j] = true;
                qr->is_func[size - 11 + j][i] = true;
            }
        }
        // Write version information bits
        int rem = qr->version;
        for (int i = 0; i < 12; i++) {
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        }
        int vbits = (qr->version << 12) | rem;
        for (int i = 0; i < 18; i++) {
            int bit = (vbits >> i) & 1;
            int a = size - 11 + (i % 3);
            int b = i / 3;
            set_module(qr, b, a, bit, true); // Top-Right block
            set_module(qr, a, b, bit, true); // Bottom-Left block
        }
    }
}

// Write the 15 format bits (error correction level M + mask) into the matrix
static void write_format_bits(qr_matrix_t *qr, int mask) {
    // ECL M format indicator is 00_2
    int data = (0 << 3) | (mask & 0x07);
    int rem = data;
    for (int i = 0; i < 10; i++) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    }
    int fbits = ((data << 10) | rem) ^ 0x5412;

    int size = qr->size;

    // First copy around top-left finder
    for (int i = 0; i <= 5; i++) {
        qr->modules[8][i] = (uint8_t)((fbits >> i) & 1);
    }
    qr->modules[8][7] = (uint8_t)((fbits >> 6) & 1);
    qr->modules[8][8] = (uint8_t)((fbits >> 7) & 1);
    qr->modules[7][8] = (uint8_t)((fbits >> 8) & 1);
    for (int i = 9; i < 15; i++) {
        qr->modules[14 - i][8] = (uint8_t)((fbits >> i) & 1);
    }

    // Second copy around top-right and bottom-left finders
    for (int i = 0; i < 8; i++) {
        qr->modules[8][size - 1 - i] = (uint8_t)((fbits >> i) & 1);
    }
    for (int i = 8; i < 15; i++) {
        qr->modules[size - 15 + i][8] = (uint8_t)((fbits >> i) & 1);
    }
}

/* ========================================================================= */
/* Zigzag Data Placement                                                     */
/* ========================================================================= */

static void place_data_bits(qr_matrix_t *qr, const uint8_t *interleaved_data, int total_codewords) {
    int total_bits = total_codewords * 8;
    int bit_idx = 0;
    int size = qr->size;

    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5; // Skip timing column 6
        }
        for (int vert = 0; vert < size; vert++) {
            for (int j = 0; j < 2; j++) {
                int c = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int r = upward ? (size - 1 - vert) : vert;

                if (!qr->is_func[r][c]) {
                    int bit = 0;
                    if (bit_idx < total_bits) {
                        int byte_idx = bit_idx / 8;
                        int bit_pos = 7 - (bit_idx % 8);
                        bit = (interleaved_data[byte_idx] >> bit_pos) & 1;
                        bit_idx++;
                    }
                    qr->modules[r][c] = (uint8_t)bit;
                }
            }
        }
    }
}

/* ========================================================================= */
/* Masking and Penalty Scoring                                               */
/* ========================================================================= */

static inline int mask_condition(int mask, int r, int c) {
    switch (mask) {
        case 0: return ((r + c) % 2) == 0;
        case 1: return (r % 2) == 0;
        case 2: return (c % 3) == 0;
        case 3: return ((r + c) % 3) == 0;
        case 4: return ((r / 2 + c / 3) % 2) == 0;
        case 5: return (((r * c) % 2) + ((r * c) % 3)) == 0;
        case 6: return ((((r * c) % 2) + ((r * c) % 3)) % 2) == 0;
        case 7: return ((((r * c) % 3) + ((r + c) % 2)) % 2) == 0;
        default: return 0;
    }
}

static void apply_mask(qr_matrix_t *qr, int mask) {
    int size = qr->size;
    for (int r = 0; r < size; r++) {
        for (int c = 0; c < size; c++) {
            if (!qr->is_func[r][c]) {
                if (mask_condition(mask, r, c)) {
                    qr->modules[r][c] ^= 1;
                }
            }
        }
    }
}

// Compute penalty score according to ISO/IEC 18004
static int calculate_penalty(const qr_matrix_t *qr) {
    int size = qr->size;
    int penalty = 0;

    // Rule 1: Runs of 5 or more same-color modules in rows and columns
    // Horizontal
    for (int r = 0; r < size; r++) {
        int current_color = qr->modules[r][0];
        int run_length = 1;
        for (int c = 1; c < size; c++) {
            if (qr->modules[r][c] == current_color) {
                run_length++;
            } else {
                if (run_length >= 5) {
                    penalty += 3 + (run_length - 5);
                }
                current_color = qr->modules[r][c];
                run_length = 1;
            }
        }
        if (run_length >= 5) {
            penalty += 3 + (run_length - 5);
        }
    }

    // Vertical
    for (int c = 0; c < size; c++) {
        int current_color = qr->modules[0][c];
        int run_length = 1;
        for (int r = 1; r < size; r++) {
            if (qr->modules[r][c] == current_color) {
                run_length++;
            } else {
                if (run_length >= 5) {
                    penalty += 3 + (run_length - 5);
                }
                current_color = qr->modules[r][c];
                run_length = 1;
            }
        }
        if (run_length >= 5) {
            penalty += 3 + (run_length - 5);
        }
    }

    // Rule 2: 2x2 blocks of modules with same color
    for (int r = 0; r < size - 1; r++) {
        for (int c = 0; c < size - 1; c++) {
            int color = qr->modules[r][c];
            if (qr->modules[r + 1][c] == color &&
                qr->modules[r][c + 1] == color &&
                qr->modules[r + 1][c + 1] == color) {
                penalty += 3;
            }
        }
    }

    // Rule 3: Finder-like patterns (1:1:3:1:1 with 4 light modules on either side)
    // Pattern A: 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0
    // Pattern B: 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1
    static const uint8_t pat_a[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
    static const uint8_t pat_b[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};

    // Rows
    for (int r = 0; r < size; r++) {
        for (int c = 0; c <= size - 11; c++) {
            bool match_a = true;
            bool match_b = true;
            for (int k = 0; k < 11; k++) {
                if (qr->modules[r][c + k] != pat_a[k]) match_a = false;
                if (qr->modules[r][c + k] != pat_b[k]) match_b = false;
                if (!match_a && !match_b) break;
            }
            if (match_a) penalty += 40;
            if (match_b) penalty += 40;
        }
    }

    // Columns
    for (int c = 0; c < size; c++) {
        for (int r = 0; r <= size - 11; r++) {
            bool match_a = true;
            bool match_b = true;
            for (int k = 0; k < 11; k++) {
                if (qr->modules[r + k][c] != pat_a[k]) match_a = false;
                if (qr->modules[r + k][c] != pat_b[k]) match_b = false;
                if (!match_a && !match_b) break;
            }
            if (match_a) penalty += 40;
            if (match_b) penalty += 40;
        }
    }

    // Rule 4: Proportion of dark modules
    int dark_count = 0;
    for (int r = 0; r < size; r++) {
        for (int c = 0; c < size; c++) {
            if (qr->modules[r][c]) {
                dark_count++;
            }
        }
    }
    int total_modules = size * size;
    // Percentage * 100 to avoid floats
    int pct = (dark_count * 100) / total_modules;
    int prev_mult = (pct / 5) * 5;
    int next_mult = prev_mult + 5;
    int dev1 = abs(prev_mult - 50) / 5;
    int dev2 = abs(next_mult - 50) / 5;
    penalty += (dev1 < dev2 ? dev1 : dev2) * 10;

    return penalty;
}

/* ========================================================================= */
/* Public API Implementation                                                 */
/* ========================================================================= */

int qr_generate(const char *data, uint8_t *matrix, size_t matrix_buf_size) {
    if (!data || !matrix || data[0] == '\0') {
        return -1;
    }

    gf_init();

    size_t data_len = strlen(data);

    // 1. Determine smallest QR version (1..10) that can fit data
    int version = -1;
    for (int v = QR_MIN_VERSION; v <= QR_MAX_VERSION; v++) {
        const qr_version_info_t *vi = &QR_VERSIONS[v];
        int char_count_bits = (v < 10) ? 8 : 16;
        int required_bits = 4 + char_count_bits + (int)(data_len * 8);
        if (required_bits <= vi->data_codewords * 8) {
            version = v;
            break;
        }
    }

    if (version < 0) {
        return -1; // Data too long for supported versions 1-10
    }

    const qr_version_info_t *vi = &QR_VERSIONS[version];
    if (matrix_buf_size < (size_t)(vi->size * vi->size)) {
        return -1; // Output buffer too small
    }

    // 2. Encode payload in Byte Mode
    uint8_t data_codewords[300];
    bit_stream_t bs;
    bs_init(&bs, data_codewords, (size_t)vi->data_codewords);

    // Mode indicator: Byte mode = 0100_2
    bs_append_bits(&bs, 0x4, 4);

    // Character count indicator: 8 bits for V1-V9, 16 bits for V10
    int char_count_bits = (version < 10) ? 8 : 16;
    bs_append_bits(&bs, (uint32_t)data_len, char_count_bits);

    // Data bytes
    for (size_t i = 0; i < data_len; i++) {
        bs_append_bits(&bs, (uint8_t)data[i], 8);
    }

    // Terminator: up to 4 zero bits
    int bits_remaining = (vi->data_codewords * 8) - (int)bs.bit_len;
    int terminator_bits = (bits_remaining > 4) ? 4 : bits_remaining;
    if (terminator_bits > 0) {
        bs_append_bits(&bs, 0, terminator_bits);
    }

    // Pad to byte boundary
    if (bs.bit_len % 8 != 0) {
        bs_append_bits(&bs, 0, 8 - (int)(bs.bit_len % 8));
    }

    // Pad codewords with alternating 0xEC and 0x11
    int current_codewords = (int)(bs.bit_len / 8);
    uint8_t pad_bytes[2] = {0xEC, 0x11};
    int pad_idx = 0;
    while (current_codewords < vi->data_codewords) {
        data_codewords[current_codewords++] = pad_bytes[pad_idx];
        pad_idx ^= 1;
    }

    // 3. Compute Reed-Solomon Error Correction and interleave blocks
    uint8_t gen_poly[64];
    rs_generator_poly(vi->ec_per_block, gen_poly);

    int total_blocks = vi->group1_blocks + vi->group2_blocks;
    uint8_t block_data[8][64];
    uint8_t block_ec[8][64];
    int block_data_len[8];

    int data_offset = 0;
    for (int b = 0; b < total_blocks; b++) {
        int dlen = (b < vi->group1_blocks) ? vi->group1_data_per_block : vi->group2_data_per_block;
        block_data_len[b] = dlen;
        memcpy(block_data[b], &data_codewords[data_offset], (size_t)dlen);
        data_offset += dlen;
        rs_compute_ec(block_data[b], dlen, vi->ec_per_block, gen_poly, block_ec[b]);
    }

    // Interleave data codewords
    uint8_t interleaved[400];
    int out_idx = 0;
    int max_data_len = vi->group2_blocks > 0 ? vi->group2_data_per_block : vi->group1_data_per_block;

    for (int i = 0; i < max_data_len; i++) {
        for (int b = 0; b < total_blocks; b++) {
            if (i < block_data_len[b]) {
                interleaved[out_idx++] = block_data[b][i];
            }
        }
    }

    // Interleave error correction codewords
    for (int i = 0; i < vi->ec_per_block; i++) {
        for (int b = 0; b < total_blocks; b++) {
            interleaved[out_idx++] = block_ec[b][i];
        }
    }

    // 4. Construct base QR matrix
    qr_matrix_t base_qr;
    memset(&base_qr, 0, sizeof(base_qr));
    base_qr.version = version;
    base_qr.size = vi->size;

    setup_function_patterns(&base_qr);
    place_data_bits(&base_qr, interleaved, vi->total_codewords);

    // 5. Evaluate 8 masks and pick the lowest penalty score
    int best_mask = 0;
    int best_penalty = 1000000000;
    qr_matrix_t best_qr;

    for (int m = 0; m < 8; m++) {
        qr_matrix_t candidate = base_qr;
        apply_mask(&candidate, m);
        write_format_bits(&candidate, m);
        int p = calculate_penalty(&candidate);
        if (p < best_penalty) {
            best_penalty = p;
            best_mask = m;
            best_qr = candidate;
        }
    }

    (void)best_mask;

    // 6. Copy final matrix into caller buffer
    for (int r = 0; r < vi->size; r++) {
        for (int c = 0; c < vi->size; c++) {
            matrix[r * vi->size + c] = best_qr.modules[r][c];
        }
    }

    return vi->size;
}

int qr_print_terminal(const char *data) {
    if (!data) {
        return -1;
    }

    uint8_t matrix[QR_MAX_SIDE * QR_MAX_SIDE];
    int size = qr_generate(data, matrix, sizeof(matrix));
    if (size <= 0) {
        return -1;
    }

    // Quiet zone: 4 modules on all 4 sides
    const int quiet = 4;
    int total_width = size + quiet * 2;
    int total_height = size + quiet * 2;

    // Render using Unicode half-block characters
    // White background for light (47m), black foreground for dark (30m)
    // █ = 0xE2 0x96 0x88 (two dark modules)
    // ▀ = 0xE2 0x96 0x80 (dark top, light bottom)
    // ▄ = 0xE2 0x96 0x84 (light top, dark bottom)
    // ' ' = space (two light modules)
    for (int r = 0; r < total_height; r += 2) {
        // ANSI escape sequence: black foreground, white background
        printf("\033[30;47m");
        for (int c = 0; c < total_width; c++) {
            int top_r = r - quiet;
            int bot_r = r + 1 - quiet;
            int mat_c = c - quiet;

            int top_val = 0;
            if (top_r >= 0 && top_r < size && mat_c >= 0 && mat_c < size) {
                top_val = matrix[top_r * size + mat_c];
            }

            int bot_val = 0;
            if (bot_r >= 0 && bot_r < size && mat_c >= 0 && mat_c < size) {
                bot_val = matrix[bot_r * size + mat_c];
            }

            if (top_val && bot_val) {
                printf("\xe2\x96\x88"); // █ Both dark
            } else if (top_val && !bot_val) {
                printf("\xe2\x96\x80"); // ▀ Dark top, light bottom
            } else if (!top_val && bot_val) {
                printf("\xe2\x96\x84"); // ▄ Light top, dark bottom
            } else {
                printf(" ");            // Both light
            }
        }
        // Reset ANSI colors at end of each row
        printf("\033[0m\n");
    }

    fflush(stdout);
    return 0;
}
