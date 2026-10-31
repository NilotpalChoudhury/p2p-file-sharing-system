#include "sha1.h"
#include <cstring>
#include <cstdio>

using namespace std;

namespace {
    inline uint32_t rol(uint32_t value, int bits) {
        return (value << bits) | (value >> (32 - bits));
    }
}

Sha1::Sha1() {
    // The five fixed initial state words from the SHA-1 spec.
    h[0] = 0x67452301;
    h[1] = 0xEFCDAB89;
    h[2] = 0x98BADCFE;
    h[3] = 0x10325476;
    h[4] = 0xC3D2E1F0;
    bit_count = 0;
    buffer_len = 0;
}

// Processes exactly one 64-byte block, mixing it into h[0..4]. This is the core of the
// algorithm; everything else is just bookkeeping to get 64-byte blocks fed in here.
void Sha1::process_block(const uint8_t block[64]) {
    uint32_t w[80];

    // First 16 words come straight from the block, big-endian.
    for (int i = 0; i < 16; i++) {
        w[i] = (static_cast<uint32_t>(block[i * 4])     << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8)  |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    // Remaining 64 words are derived from earlier ones (the "message schedule").
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }

        uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void Sha1::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bit_count += static_cast<uint64_t>(len) * 8;

    while (len > 0) {
        size_t take = 64 - buffer_len;
        if (take > len) take = len;
        memcpy(buffer + buffer_len, p, take);
        buffer_len += take;
        p += take;
        len -= take;
        if (buffer_len == 64) {
            process_block(buffer);
            buffer_len = 0;
        }
    }
}

std::string Sha1::final_hex() {
   
    uint64_t saved_bit_count = bit_count;

    uint8_t pad = 0x80;
    update(&pad, 1);

    uint8_t zero = 0x00;
    while (buffer_len != 56) {
        update(&zero, 1);
    }


    uint8_t length_bytes[8];
    for (int i = 0; i < 8; i++) {
        length_bytes[i] = static_cast<uint8_t>(saved_bit_count >> (56 - 8 * i));
    }
    memcpy(buffer + 56, length_bytes, 8);
    process_block(buffer);
    buffer_len = 0;

    char out[41];
    for (int i = 0; i < 5; i++) {
        snprintf(out + i * 8, 9, "%08x", h[i]);
    }
    return std::string(out, 40);
}

std::string Sha1::hex(const void* data, size_t len) {
    Sha1 s;
    s.update(data, len);
    return s.final_hex();
}
