#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace pebble {

// A fixed-size Bloom filter: a bit vector probed at three positions per key.
// may_contain() can return false positives but never false negatives.
class BloomFilter {
public:
    static constexpr std::uint64_t kNumHashes = 3;

    explicit BloomFilter(std::size_t expected_items = 0, std::size_t bits_per_item = 10);

    void add(std::string_view key);
    bool may_contain(std::string_view key) const;
    std::size_t bit_count() const { return num_bits_; }

private:
    std::vector<std::uint64_t> words_;
    std::size_t num_bits_;
};

}  // namespace pebble
