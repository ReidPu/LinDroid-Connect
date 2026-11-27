#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace l2a {

using Sha256Digest = std::array<uint8_t, 32>;

class Sha256Hasher {
public:
    Sha256Hasher() noexcept;

    void reset() noexcept;
    void update(std::span<const uint8_t> data) noexcept;
    void update(std::string_view text) noexcept;
    Sha256Digest finalize() noexcept;

private:
    void transform(const uint8_t block[64]) noexcept;

    std::array<uint32_t, 8> state_{};
    std::array<uint8_t, 64> buffer_{};
    uint64_t bit_count_{0};
    size_t buffer_len_{0};
};

Sha256Digest sha256_bytes(std::span<const uint8_t> data) noexcept;
Sha256Digest sha256_string(std::string_view text) noexcept;

// Computes SHA-256 over a file range using a fixed 64KB streaming buffer.
// If length == 0, hashes from offset to EOF.
std::optional<Sha256Digest> sha256_file_range(
    const std::filesystem::path& file_path,
    uint64_t offset = 0,
    uint64_t length = 0);

Sha256Digest hmac_sha256(
    std::span<const uint8_t> key,
    std::span<const uint8_t> data) noexcept;

std::vector<uint8_t> hkdf_sha256(
    std::span<const uint8_t> salt,
    std::span<const uint8_t> ikm,
    std::span<const uint8_t> info,
    size_t output_len);

uint16_t crc16_ccitt(std::span<const uint8_t> data) noexcept;

std::string to_hex(std::span<const uint8_t> bytes);
std::optional<std::vector<uint8_t>> from_hex(std::string_view hex);
std::optional<Sha256Digest> digest_from_hex(std::string_view hex);

}  // namespace l2a
