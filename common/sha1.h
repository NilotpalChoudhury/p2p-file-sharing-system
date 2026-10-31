#ifndef SHA1_H
#define SHA1_H

#include <cstdint>
#include <cstddef>
#include <string>

class Sha1 {
public:
    Sha1();

    void update(const void* data, size_t len);


    std::string final_hex();

    static std::string hex(const void* data, size_t len);

private:
    void process_block(const uint8_t block[64]);

    uint32_t h[5];       // the five 32-bit hash state words
    uint64_t bit_count;  // total bits processed so far (needed for the length field in padding)
    uint8_t buffer[64];  // holds a partial 64-byte block between update() calls
    size_t buffer_len;   // how many bytes of `buffer` are currently filled
};

#endif // SHA1_H
