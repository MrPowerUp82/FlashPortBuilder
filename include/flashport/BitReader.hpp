#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace flashport {

// MSB-first bit reader used by SWF RECT, MATRIX, CXFORM and shape records.
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size, std::size_t byteOffset = 0)
        : data_(data), size_(size), bitPos_(byteOffset * 8) {}
    BitReader(const std::vector<std::uint8_t>& data, std::size_t byteOffset)
        : BitReader(data.data(), data.size(), byteOffset) {}

    std::uint32_t bits(unsigned n) {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            const std::size_t byte = bitPos_ / 8;
            if (byte >= size_) throw std::runtime_error("unexpected end of bit field");
            const unsigned shift = 7u - static_cast<unsigned>(bitPos_ % 8);
            v = (v << 1) | ((data_[byte] >> shift) & 1u);
            ++bitPos_;
        }
        return v;
    }

    std::int32_t signedBits(unsigned n) {
        if (n == 0) return 0;
        const auto raw = bits(n);
        if (n < 32 && (raw & (1u << (n - 1))) != 0) {
            return static_cast<std::int32_t>(raw | (~0u << n));
        }
        return static_cast<std::int32_t>(raw);
    }

    // 16.16 fixed point stored in n bits.
    double fixedBits(unsigned n) { return static_cast<double>(signedBits(n)) / 65536.0; }

    bool flag() { return bits(1) != 0; }
    void align() { bitPos_ = (bitPos_ + 7) / 8 * 8; }
    std::size_t nextByte() const { return (bitPos_ + 7) / 8; }

    std::uint8_t u8() { align(); return static_cast<std::uint8_t>(bits(8)); }
    std::uint16_t u16() {
        align();
        const std::uint16_t lo = static_cast<std::uint16_t>(bits(8));
        const std::uint16_t hi = static_cast<std::uint16_t>(bits(8));
        return static_cast<std::uint16_t>(lo | (hi << 8));
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t bitPos_{};
};

} // namespace flashport
