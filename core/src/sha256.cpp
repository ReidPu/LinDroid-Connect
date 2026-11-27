#include "l2a/sha256.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace l2a {

namespace {

constexpr std::array<uint32_t, 64> kSha256K = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

constexpr uint32_t rotr(uint32_t x, uint32_t n) noexcept {
    return (x >> n) | (x << (32U - n));
}

}  // namespace

Sha256Hasher::Sha256Hasher() noexcept {
    reset();
}

void Sha256Hasher::reset() noexcept {
    state_ = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    bit_count_ = 0;
    buffer_len_ = 0;
}

void Sha256Hasher::transform(const uint8_t block[64]) noexcept {
    uint32_t w[64];
    for (size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24U) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16U) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8U) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (size_t i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];

    for (size_t i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256Hasher::update(std::span<const uint8_t> data) noexcept {
    const uint8_t* ptr = data.data();
    size_t remaining = data.size();
    bit_count_ += static_cast<uint64_t>(remaining) * 8ULL;

    if (buffer_len_ > 0) {
        size_t to_copy = std::min(64 - buffer_len_, remaining);
        std::memcpy(buffer_.data() + buffer_len_, ptr, to_copy);
        buffer_len_ += to_copy;
        ptr += to_copy;
        remaining -= to_copy;
        if (buffer_len_ == 64) {
            transform(buffer_.data());
            buffer_len_ = 0;
        }
    }

    while (remaining >= 64) {
        transform(ptr);
        ptr += 64;
        remaining -= 64;
    }

    if (remaining > 0) {
        std::memcpy(buffer_.data(), ptr, remaining);
        buffer_len_ = remaining;
    }
}

void Sha256Hasher::update(std::string_view text) noexcept {
    update(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

Sha256Digest Sha256Hasher::finalize() noexcept {
    buffer_[buffer_len_++] = 0x80U;
    if (buffer_len_ > 56) {
        while (buffer_len_ < 64) {
            buffer_[buffer_len_++] = 0x00U;
        }
        transform(buffer_.data());
        buffer_len_ = 0;
    }
    while (buffer_len_ < 56) {
        buffer_[buffer_len_++] = 0x00U;
    }
    for (int i = 7; i >= 0; --i) {
        buffer_[buffer_len_++] = static_cast<uint8_t>((bit_count_ >> (i * 8)) & 0xFFU);
    }
    transform(buffer_.data());

    Sha256Digest digest{};
    for (size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<uint8_t>((state_[i] >> 24U) & 0xFFU);
        digest[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16U) & 0xFFU);
        digest[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8U) & 0xFFU);
        digest[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFFU);
    }
    reset();
    return digest;
}

Sha256Digest sha256_bytes(std::span<const uint8_t> data) noexcept {
    Sha256Hasher hasher;
    hasher.update(data);
    return hasher.finalize();
}

Sha256Digest sha256_string(std::string_view text) noexcept {
    Sha256Hasher hasher;
    hasher.update(text);
    return hasher.finalize();
}

std::optional<Sha256Digest> sha256_file_range(
    const std::filesystem::path& file_path,
    uint64_t offset,
    uint64_t length) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }

    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file.good()) {
        return std::nullopt;
    }

    Sha256Hasher hasher;
    std::array<uint8_t, 65536> chunk_buf{};
    uint64_t bytes_left = length;
    const bool bounded = (length > 0);

    while (file.good()) {
        size_t to_read = chunk_buf.size();
        if (bounded) {
            if (bytes_left == 0) {
                break;
            }
            to_read = static_cast<size_t>(
                std::min<uint64_t>(chunk_buf.size(), bytes_left));
        }

        file.read(reinterpret_cast<char*>(chunk_buf.data()),
                  static_cast<std::streamsize>(to_read));
        std::streamsize got = file.gcount();
        if (got <= 0) {
            break;
        }
        hasher.update(std::span<const uint8_t>(
            chunk_buf.data(), static_cast<size_t>(got)));
        if (bounded) {
            bytes_left -= static_cast<uint64_t>(got);
        }
    }

    if (bounded && bytes_left != 0) {
        return std::nullopt;
    }

    return hasher.finalize();
}

Sha256Digest hmac_sha256(
    std::span<const uint8_t> key,
    std::span<const uint8_t> data) noexcept {
    std::array<uint8_t, 64> k_pad{};
    if (key.size() > 64) {
        Sha256Digest hashed_key = sha256_bytes(key);
        std::memcpy(k_pad.data(), hashed_key.data(), hashed_key.size());
    } else if (!key.empty()) {
        std::memcpy(k_pad.data(), key.data(), key.size());
    }

    std::array<uint8_t, 64> i_key_pad{};
    std::array<uint8_t, 64> o_key_pad{};
    for (size_t i = 0; i < 64; ++i) {
        i_key_pad[i] = k_pad[i] ^ 0x36U;
        o_key_pad[i] = k_pad[i] ^ 0x5cU;
    }

    Sha256Hasher inner;
    inner.update(i_key_pad);
    inner.update(data);
    Sha256Digest inner_digest = inner.finalize();

    Sha256Hasher outer;
    outer.update(o_key_pad);
    outer.update(inner_digest);
    return outer.finalize();
}

std::vector<uint8_t> hkdf_sha256(
    std::span<const uint8_t> salt,
    std::span<const uint8_t> ikm,
    std::span<const uint8_t> info,
    size_t output_len) {
    static constexpr std::array<uint8_t, 32> kZeroSalt{};
    std::span<const uint8_t> actual_salt =
        salt.empty() ? std::span<const uint8_t>(kZeroSalt) : salt;

    Sha256Digest prk = hmac_sha256(actual_salt, ikm);

    std::vector<uint8_t> okm;
    okm.reserve(output_len);
    std::vector<uint8_t> prev;
    uint8_t counter = 1;

    while (okm.size() < output_len) {
        std::vector<uint8_t> hmac_input;
        hmac_input.reserve(prev.size() + info.size() + 1);
        hmac_input.insert(hmac_input.end(), prev.begin(), prev.end());
        hmac_input.insert(hmac_input.end(), info.begin(), info.end());
        hmac_input.push_back(counter++);

        Sha256Digest block = hmac_sha256(prk, hmac_input);
        prev.assign(block.begin(), block.end());

        size_t need = std::min(output_len - okm.size(), block.size());
        okm.insert(okm.end(), block.begin(), block.begin() + need);
    }

    return okm;
}

uint16_t crc16_ccitt(std::span<const uint8_t> data) noexcept {
    uint16_t crc = 0xFFFFU;
    for (uint8_t byte : data) {
        crc ^= static_cast<uint16_t>(byte) << 8U;
        for (int i = 0; i < 8; ++i) {
            if ((crc & 0x8000U) != 0) {
                crc = static_cast<uint16_t>((crc << 1U) ^ 0x1021U);
            } else {
                crc = static_cast<uint16_t>(crc << 1U);
            }
        }
    }
    return crc;
}

std::string to_hex(std::span<const uint8_t> bytes) {
    static constexpr char kHexChars[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = kHexChars[(bytes[i] >> 4U) & 0x0FU];
        out[i * 2 + 1] = kHexChars[bytes[i] & 0x0FU];
    }
    return out;
}

std::optional<std::vector<uint8_t>> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

std::optional<Sha256Digest> digest_from_hex(std::string_view hex) {
    auto bytes = from_hex(hex);
    if (!bytes || bytes->size() != 32) {
        return std::nullopt;
    }
    Sha256Digest d{};
    std::memcpy(d.data(), bytes->data(), 32);
    return d;
}

}  // namespace l2a
