#pragma once

#include "l2a/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace l2a {

constexpr uint16_t kProtocolMagic = 0x4C32U;  // "L2"
constexpr uint8_t kProtocolVersion = 0x01U;
constexpr size_t kFrameHeaderSize = 16;
constexpr size_t kMaxChunkSize = 65536;               // 64 KB fixed streaming slice
constexpr size_t kMaxFramePayload = kMaxChunkSize + 1024;  // 64 KB + metadata overhead

enum class FrameType : uint8_t {
    kHelloHandshake   = 0x01,
    kPairRequest      = 0x02,
    kPairResponse     = 0x04,
    kPing             = 0x03,
    kPong             = 0x05,

    kClipOffer        = 0x10,
    kClipDataChunk    = 0x11,
    kClipPullReq      = 0x12,

    kFileBatchMeta    = 0x20,
    kFileResumeOffset = 0x21,
    kFileDataChunk    = 0x22,
    kFileVerifiedAck  = 0x23,
};

namespace FrameFlags {
constexpr uint8_t kNone        = 0x00;
constexpr uint8_t kFin         = 0x01;
constexpr uint8_t kCompressed  = 0x02;
constexpr uint8_t kAckRequired = 0x04;
constexpr uint8_t kPullResponse = 0x08;
}  // namespace FrameFlags

struct FrameHeader {
    uint16_t magic{kProtocolMagic};
    uint8_t version{kProtocolVersion};
    FrameType type{FrameType::kPing};
    uint8_t flags{FrameFlags::kNone};
    uint8_t reserved{0};
    uint32_t stream_id{0};
    uint32_t payload_length{0};
    uint16_t header_crc16{0};

    std::array<uint8_t, kFrameHeaderSize> encode() const noexcept;
    static std::optional<FrameHeader> decode(
        std::span<const uint8_t, kFrameHeaderSize> raw) noexcept;
};

struct Frame {
    FrameHeader header{};
    std::vector<uint8_t> payload{};

    std::vector<uint8_t> serialize() const;
    static std::optional<Frame> deserialize(std::span<const uint8_t> raw);
};

// --- Control & Pairing Payloads ---

struct HelloHandshakePayload {
    std::string device_id;
    std::string device_name;
    std::string platform;  // "linux" or "android"
    uint8_t protocol_version{kProtocolVersion};
    Sha256Digest pubkey_fingerprint{};

    std::vector<uint8_t> encode() const;
    static std::optional<HelloHandshakePayload> decode(std::span<const uint8_t> data);
};

struct PairRequestPayload {
    std::string device_id;
    std::string device_name;
    std::string pin_or_token;
    Sha256Digest pubkey_fingerprint{};

    std::vector<uint8_t> encode() const;
    static std::optional<PairRequestPayload> decode(std::span<const uint8_t> data);
};

struct PairResponsePayload {
    std::string device_id;
    bool accepted{false};
    Sha256Digest pubkey_fingerprint{};
    std::string message;

    std::vector<uint8_t> encode() const;
    static std::optional<PairResponsePayload> decode(std::span<const uint8_t> data);
};

// --- Clipboard Streaming Payloads ---

struct ClipOfferPayload {
    uint64_t clip_id{0};
    std::string mime_type;  // e.g. "text/plain", "text/html", "image/png", "text/uri-list"
    uint64_t total_size{0};
    Sha256Digest sha256{};
    bool lazy_pull{false};  // true if receiver should send CLIP_PULL_REQ on demand

    std::vector<uint8_t> encode() const;
    static std::optional<ClipOfferPayload> decode(std::span<const uint8_t> data);
};

struct ClipDataChunkPayload {
    uint64_t clip_id{0};
    uint64_t offset{0};
    std::vector<uint8_t> data;  // <= 64 KB

    std::vector<uint8_t> encode() const;
    static std::optional<ClipDataChunkPayload> decode(std::span<const uint8_t> data);
};

struct ClipPullReqPayload {
    uint64_t clip_id{0};

    std::vector<uint8_t> encode() const;
    static std::optional<ClipPullReqPayload> decode(std::span<const uint8_t> data);
};

// --- File Transfer & Resume Payloads ---

struct FileEntryMeta {
    uint32_t file_index{0};
    std::string relative_path;
    uint64_t file_size{0};
    uint64_t last_modified_ms{0};
    Sha256Digest file_sha256{};
};

struct FileBatchMetaPayload {
    uint64_t batch_id{0};
    uint32_t total_files{0};
    uint64_t total_bytes{0};
    std::vector<FileEntryMeta> files;

    std::vector<uint8_t> encode() const;
    static std::optional<FileBatchMetaPayload> decode(std::span<const uint8_t> data);
};

struct FileResumeEntry {
    uint32_t file_index{0};
    uint64_t resume_offset{0};
    Sha256Digest tail_block_sha256{};  // SHA-256 of last <=64KB block before resume_offset
};

struct FileResumeOffsetPayload {
    uint64_t batch_id{0};
    std::vector<FileResumeEntry> entries;

    std::vector<uint8_t> encode() const;
    static std::optional<FileResumeOffsetPayload> decode(std::span<const uint8_t> data);
};

struct FileDataChunkPayload {
    uint64_t batch_id{0};
    uint32_t file_index{0};
    uint64_t offset{0};
    std::vector<uint8_t> data;  // <= 64 KB

    std::vector<uint8_t> encode() const;
    static std::optional<FileDataChunkPayload> decode(std::span<const uint8_t> data);
};

struct FileVerifiedAckPayload {
    uint64_t batch_id{0};
    uint32_t file_index{0};
    bool success{false};
    std::string error_message;

    std::vector<uint8_t> encode() const;
    static std::optional<FileVerifiedAckPayload> decode(std::span<const uint8_t> data);
};

}  // namespace l2a
