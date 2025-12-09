#include "bloom_filter.hpp"

#include <algorithm>

namespace pebble {
namespace {

// FNV-1a: a simple, stable 64-bit string hash.
std::uint64_t fnv1a(std::string_view s) {
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// SplitMix64 finalizer: scrambles bits so nearby keys land far apart.
std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// The three hash functions are h1 + i * h2 for i = 0, 1, 2 (double hashing).
struct HashPair {
    std::uint64_t h1;
    std::uint64_t h2;
};

HashPair hash_pair(std::string_view key) {
    const std::uint64_t base = fnv1a(key);
    return {mix(base), mix(base ^ 0x9e3779b97f4a7c15ULL) | 1};
}

}  // namespace

BloomFilter::BloomFilter(std::size_t expected_items, std::size_t bits_per_item)
    : num_bits_(std::max<std::size_t>(64, expected_items * bits_per_item)) {
    words_.assign((num_bits_ + 63) / 64, 0);
}

void BloomFilter::add(std::string_view key) {
    const HashPair h = hash_pair(key);
    for (std::uint64_t i = 0; i < kNumHashes; ++i) {
        const std::uint64_t bit = (h.h1 + i * h.h2) % num_bits_;
        words_[bit / 64] |= (1ULL << (bit % 64));
    }
}

bool BloomFilter::may_contain(std::string_view key) const {
    const HashPair h = hash_pair(key);
    for (std::uint64_t i = 0; i < kNumHashes; ++i) {
        const std::uint64_t bit = (h.h1 + i * h.h2) % num_bits_;
        if ((words_[bit / 64] & (1ULL << (bit % 64))) == 0) return false;
    }
    return true;
}

}  // namespace pebble
