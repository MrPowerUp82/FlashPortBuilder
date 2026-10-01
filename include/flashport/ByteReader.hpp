#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace flashport {

class ByteReader {
public:
    ByteReader(const std::vector<std::uint8_t>& data, std::size_t offset = 0)
        : data_(data), pos_(offset) {}

    std::size_t pos() const { return pos_; }
    std::size_t remaining() const { return data_.size() - pos_; }
    bool eof() const { return pos_ >= data_.size(); }

    void seek(std::size_t p) {
        if (p > data_.size()) throw std::runtime_error("seek past end");
        pos_ = p;
    }

    void skip(std::size_t n) { seek(pos_ + n); }

    std::uint8_t u8() {
        require(1); return data_[pos_++];
    }

    std::uint16_t u16() {
        require(2);
        std::uint16_t v = static_cast<std::uint16_t>(data_[pos_]) |
                          (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
        pos_ += 2; return v;
    }

    std::uint32_t u32() {
        require(4);
        std::uint32_t v = static_cast<std::uint32_t>(data_[pos_]) |
                          (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8) |
                          (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16) |
                          (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4; return v;
    }

    std::uint32_t u30() {
        std::uint32_t result = 0;
        for (int i = 0; i < 5; ++i) {
            std::uint8_t b = u8();
            result |= static_cast<std::uint32_t>(b & 0x7f) << (7 * i);
            if ((b & 0x80) == 0) return result;
        }
        return result;
    }

    std::int32_t s32() { return static_cast<std::int32_t>(u30()); }

    double f64() {
        require(8);
        std::uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) raw |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i);
        pos_ += 8;
        double d;
        std::memcpy(&d, &raw, sizeof(d));
        return d;
    }

    std::string cstring() {
        std::string out;
        while (true) {
            auto c = u8();
            if (c == 0) break;
            out.push_back(static_cast<char>(c));
        }
        return out;
    }

    std::string bytesAsString(std::size_t n) {
        require(n);
        std::string out(reinterpret_cast<const char*>(data_.data() + pos_), n);
        pos_ += n;
        return out;
    }

    std::vector<std::uint8_t> bytes(std::size_t n) {
        require(n);
        std::vector<std::uint8_t> out(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                      data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return out;
    }

private:
    void require(std::size_t n) const {
        if (pos_ + n > data_.size()) throw std::runtime_error("unexpected end of data");
    }

    const std::vector<std::uint8_t>& data_;
    std::size_t pos_{};
};

} // namespace flashport
