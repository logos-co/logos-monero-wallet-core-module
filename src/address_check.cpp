#include "address_check.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// ── Keccak-256 (original Keccak padding 0x01, as Monero uses) ──────────────
constexpr uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
constexpr int ROT[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
constexpr int PIL[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

inline uint64_t rotl(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

void keccakf(uint64_t st[25]) {
    for (int r = 0; r < 24; ++r) {
        uint64_t bc[5];
        for (int i = 0; i < 5; ++i) bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        for (int i = 0; i < 5; ++i) {
            const uint64_t t = bc[(i + 4) % 5] ^ rotl(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5) st[j + i] ^= t;
        }
        uint64_t t = st[1];
        for (int i = 0; i < 24; ++i) { const int j = PIL[i]; const uint64_t tmp = st[j]; st[j] = rotl(t, ROT[i]); t = tmp; }
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; ++i) bc[i] = st[j + i];
            for (int i = 0; i < 5; ++i) st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }
        st[0] ^= RC[r];
    }
}

void keccak256(const uint8_t* in, size_t len, uint8_t out[32]) {
    uint64_t st[25] = {0};
    const size_t rate = 136;
    uint8_t tmp[136];
    while (len >= rate) {
        for (size_t i = 0; i < rate / 8; ++i) { uint64_t w; memcpy(&w, in + i * 8, 8); st[i] ^= w; }
        keccakf(st); in += rate; len -= rate;
    }
    memset(tmp, 0, rate); memcpy(tmp, in, len);
    tmp[len] |= 0x01; tmp[rate - 1] |= 0x80;
    for (size_t i = 0; i < rate / 8; ++i) { uint64_t w; memcpy(&w, tmp + i * 8, 8); st[i] ^= w; }
    keccakf(st);
    memcpy(out, st, 32);
}

// ── Monero base58: 8-byte blocks encode to 11 chars, the tail block shorter ──
constexpr const char* ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr int ENC_SIZES[9] = {0, 2, 3, 5, 6, 7, 9, 10, 11};

bool decodeBlock(const char* s, size_t n, std::vector<uint8_t>& out) {
    int size = -1;
    for (int i = 0; i < 9; ++i) if (ENC_SIZES[i] == static_cast<int>(n)) size = i;
    if (size <= 0) return false;
    __uint128_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        const char* p = strchr(ALPHABET, s[i]);
        if (!p || s[i] == 0) return false;
        v = v * 58 + static_cast<unsigned>(p - ALPHABET);
    }
    if (size < 8 && v >= (static_cast<__uint128_t>(1) << (8 * size))) return false;
    for (int i = size - 1; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return true;
}

bool base58Decode(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); i += 11)
        if (!decodeBlock(s.data() + i, std::min<size_t>(11, s.size() - i), out)) return false;
    return true;
}

bool readVarint(const std::vector<uint8_t>& b, uint64_t& v, size_t& n) {
    v = 0; n = 0;
    for (int shift = 0; n < b.size() && shift < 64; shift += 7) {
        v |= static_cast<uint64_t>(b[n] & 0x7f) << shift;
        if (!(b[n++] & 0x80)) return true;
    }
    return false;
}

} // namespace

namespace monero_addr {

bool valid(const std::string& address, const std::string& network) {
    // {standard, subaddress, integrated}
    uint64_t std_p, sub_p, int_p;
    if (network == "mainnet" || network == "regtest") { std_p = 18; sub_p = 42; int_p = 19; }
    else if (network == "testnet")  { std_p = 53; sub_p = 63; int_p = 54; }
    else if (network == "stagenet") { std_p = 24; sub_p = 36; int_p = 25; }
    else return false;

    std::vector<uint8_t> b;
    if (address.size() < 90 || !base58Decode(address, b) || b.size() < 4) return false;
    uint64_t prefix; size_t n;
    if (!readVarint(b, prefix, n)) return false;
    const size_t body = b.size() - 4;
    if (prefix == std_p || prefix == sub_p) { if (body != n + 64) return false; }
    else if (prefix == int_p)               { if (body != n + 72) return false; }
    else return false;

    uint8_t h[32];
    keccak256(b.data(), body, h);
    return memcmp(h, b.data() + body, 4) == 0;
}

} // namespace monero_addr
