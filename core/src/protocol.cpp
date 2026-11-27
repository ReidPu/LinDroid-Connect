#include "l2a/protocol.hpp"

#include <cstring>

namespace l2a {

namespace {

class ByteWriter {
public:
    void write_u8(uint8_t v) { buf_.push_back(v); }

    void write_u16(uint16_t v) {
        buf_.push_back(static_cast<uint8_t>((v >> 8U) & 0xFFU));
        buf_.push_back(static_cast<uint8_t>(v & 0xFFU));
    }

    void write_u32(uint32_t v) {
        buf_.push_back(static_cast<uint8_t>((v >> 24U) & 0xFFU));
        buf_.push_back(static_cast<uint8_t>((v >> 16U) & 0xFFU));
        buf_.push_back(static_cast<uint8_t>((v >> 8U) & 0xFFU));
        buf_.push_back(static_cast<uint8_t>(v & 0xFFU));
    }

    void write_u64(uint64_t v) {
        for (int i = 7; i >= 0; --i) {
            buf_.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFU));
        }
    }

    void write_bytes(std::span<const uint8_t> data) {
        buf_.insert(buf_.end(), data.begin(), data.end());
    }

    void write_string(std::string_view s) {
        write_u16(static_cast<uint16_t>(s.size()));
        buf_.insert(buf_.end(),
                    reinterpret_cast<const uint8_t*>(s.data()),
                    reinterpret_cast<const uint8_t*>(s.data()) + s.size());
    }

    void write_blob(std::span<const uint8_t> blob) {
        write_u32(static_cast<uint32_t>(blob.size()));
        write_bytes(blob);
    }

    std::vector<uint8_t> take() { return std::move(buf_); }

private:
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> data) : data_(data) {}

    bool read_u8(uint8_t& out) {
        if (pos_ + 1 > data_.size()) return false;
        out = data_[pos_++];
        return true;
    }

    bool read_u16(uint16_t& out) {
        if (pos_ + 2 > data_.size()) return false;
        out = static_cast<uint16_t>((static_cast<uint16_t>(data_[pos_]) << 8U) |
                                    static_cast<uint16_t>(data_[pos_ + 1]));
        pos_ += 2;
        return true;
    }

    bool read_u32(uint32_t& out) {
        if (pos_ + 4 > data_.size()) return false;
        out = (static_cast<uint32_t>(data_[pos_]) << 24U) |
              (static_cast<uint32_t>(data_[pos_ + 1]) << 16U) |
              (static_cast<uint32_t>(data_[pos_ + 2]) << 8U) |
              static_cast<uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return true;
    }

    bool read_u64(uint64_t& out) {
        if (pos_ + 8 > data_.size()) return false;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8U) | static_cast<uint64_t>(data_[pos_ + i]);
        }
        pos_ += 8;
        out = v;
        return true;
    }

    bool read_bytes(std::span<uint8_t> out) {
        if (pos_ + out.size() > data_.size()) return false;
        std::memcpy(out.data(), data_.data() + pos_, out.size());
        pos_ += out.size();
        return true;
    }

    bool read_string(std::string& out) {
        uint16_t len = 0;
        if (!read_u16(len)) return false;
        if (pos_ + len > data_.size()) return false;
        out.assign(reinterpret_cast<const char*>(data_.data() + pos_), len);
        pos_ += len;
        return true;
    }

    bool read_blob(std::vector<uint8_t>& out, size_t max_len = kMaxChunkSize) {
        uint32_t len = 0;
        if (!read_u32(len)) return false;
        if (len > max_len || pos_ + len > data_.size()) return false;
        out.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                   data_.begin() + static_cast<std::ptrdiff_t>(pos_ + len));
        pos_ += len;
        return true;
    }

    bool empty() const noexcept { return pos_ == data_.size(); }
    size_t remaining() const noexcept { return pos_ <= data_.size() ? (data_.size() - pos_) : 0; }

private:
    std::span<const uint8_t> data_;
    size_t pos_{0};
};

}  // namespace

std::array<uint8_t, kFrameHeaderSize> FrameHeader::encode() const noexcept {
    std::array<uint8_t, kFrameHeaderSize> raw{};
    raw[0] = static_cast<uint8_t>((magic >> 8U) & 0xFFU);
    raw[1] = static_cast<uint8_t>(magic & 0xFFU);
    raw[2] = version;
    raw[3] = static_cast<uint8_t>(type);
    raw[4] = flags;
    raw[5] = reserved;
    raw[6] = static_cast<uint8_t>((stream_id >> 24U) & 0xFFU);
    raw[7] = static_cast<uint8_t>((stream_id >> 16U) & 0xFFU);
    raw[8] = static_cast<uint8_t>((stream_id >> 8U) & 0xFFU);
    raw[9] = static_cast<uint8_t>(stream_id & 0xFFU);
    raw[10] = static_cast<uint8_t>((payload_length >> 24U) & 0xFFU);
    raw[11] = static_cast<uint8_t>((payload_length >> 16U) & 0xFFU);
    raw[12] = static_cast<uint8_t>((payload_length >> 8U) & 0xFFU);
    raw[13] = static_cast<uint8_t>(payload_length & 0xFFU);

    uint16_t crc = crc16_ccitt(std::span<const uint8_t>(raw.data(), 14));
    raw[14] = static_cast<uint8_t>((crc >> 8U) & 0xFFU);
    raw[15] = static_cast<uint8_t>(crc & 0xFFU);
    return raw;
}

std::optional<FrameHeader> FrameHeader::decode(
    std::span<const uint8_t, kFrameHeaderSize> raw) noexcept {
    FrameHeader h{};
    h.magic = static_cast<uint16_t>((static_cast<uint16_t>(raw[0]) << 8U) | raw[1]);
    if (h.magic != kProtocolMagic) {
        return std::nullopt;
    }
    h.version = raw[2];
    if (h.version != kProtocolVersion) {
        return std::nullopt;
    }
    h.type = static_cast<FrameType>(raw[3]);
    h.flags = raw[4];
    h.reserved = raw[5];
    h.stream_id = (static_cast<uint32_t>(raw[6]) << 24U) |
                  (static_cast<uint32_t>(raw[7]) << 16U) |
                  (static_cast<uint32_t>(raw[8]) << 8U) |
                  static_cast<uint32_t>(raw[9]);
    h.payload_length = (static_cast<uint32_t>(raw[10]) << 24U) |
                       (static_cast<uint32_t>(raw[11]) << 16U) |
                       (static_cast<uint32_t>(raw[12]) << 8U) |
                       static_cast<uint32_t>(raw[13]);
    h.header_crc16 = static_cast<uint16_t>((static_cast<uint16_t>(raw[14]) << 8U) | raw[15]);

    uint16_t expected_crc = crc16_ccitt(std::span<const uint8_t>(raw.data(), 14));
    if (h.header_crc16 != expected_crc) {
        return std::nullopt;
    }
    if (h.payload_length > kMaxFramePayload) {
        return std::nullopt;
    }
    return h;
}

std::vector<uint8_t> Frame::serialize() const {
    FrameHeader h = header;
    h.payload_length = static_cast<uint32_t>(payload.size());
    auto raw_hdr = h.encode();

    std::vector<uint8_t> out;
    out.reserve(kFrameHeaderSize + payload.size());
    out.insert(out.end(), raw_hdr.begin(), raw_hdr.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<Frame> Frame::deserialize(std::span<const uint8_t> raw) {
    if (raw.size() < kFrameHeaderSize) {
        return std::nullopt;
    }
    std::span<const uint8_t, kFrameHeaderSize> hdr_span(raw.data(), kFrameHeaderSize);
    auto hdr = FrameHeader::decode(hdr_span);
    if (!hdr) {
        return std::nullopt;
    }
    if (raw.size() != kFrameHeaderSize + hdr->payload_length) {
        return std::nullopt;
    }
    Frame f{};
    f.header = *hdr;
    f.payload.assign(raw.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize), raw.end());
    return f;
}

// --- HelloHandshakePayload ---

std::vector<uint8_t> HelloHandshakePayload::encode() const {
    ByteWriter w;
    w.write_string(device_id);
    w.write_string(device_name);
    w.write_string(platform);
    w.write_u8(protocol_version);
    w.write_bytes(pubkey_fingerprint);
    return w.take();
}

std::optional<HelloHandshakePayload> HelloHandshakePayload::decode(std::span<const uint8_t> data) {
    ByteReader r(data);
    HelloHandshakePayload p{};
    if (!r.read_string(p.device_id) ||
        !r.read_string(p.device_name) ||
        !r.read_string(p.platform) ||
        !r.read_u8(p.protocol_version) ||
        !r.read_bytes(p.pubkey_fingerprint)) {
        return std::nullopt;
    }
    return p;
}

// --- PairRequestPayload ---

std::vector<uint8_t> PairRequestPayload::encode() const {
    ByteWriter w;
    w.write_string(device_id);
    w.write_string(device_name);
    w.write_string(pin_or_token);
    w.write_bytes(pubkey_fingerprint);
    return w.take();
}

std::optional<PairRequestPayload> PairRequestPayload::decode(std::span<const uint8_t> data) {
    ByteReader r(data);
    PairRequestPayload p{};
    if (!r.read_string(p.device_id) ||
        !r.read_string(p.device_name) ||
        !r.read_string(p.pin_or_token) ||
        !r.read_bytes(p.pubkey_fingerprint)) {
        return std::nullopt;
    }
    return p;
}

// --- PairResponsePayload ---

std::vector<uint8_t> PairResponsePayload::encode() const {
    ByteWriter w;
    w.write_string(device_id);
    w.write_u8(accepted ? 1 : 0);
    w.write_bytes(pubkey_fingerprint);
    w.write_string(message);
    return w.take();
}

std::optional<PairResponsePayload> PairResponsePayload::decode(std::span<const uint8_t> data) {
    ByteReader r(data);
    PairResponsePayload p{};
    uint8_t acc = 0;
    if (!r.read_string(p.device_id) ||
        !r.read_u8(acc) ||
        !r.read_bytes(p.pubkey_fingerprint) ||
        !r.read_string(p.message)) {
        return std::nullopt;
    }
    p.accepted = (acc != 0);
    return p;
}

// --- ClipOfferPayload ---

std::vector<uint8_t> ClipOfferPayload::encode() const {
    ByteWriter w;
    w.write_u64(clip_id);
    w.write_string(mime_type);
    w.write_u64(total_size);
    w.write_bytes(sha256);
    w.write_u8(lazy_pull ? 1 : 0);
    return w.take();
}

std::optional<ClipOfferPayload> ClipOfferPayload::decode(std::span<const uint8_t> data) {
    ByteReader r(data);
    ClipOfferPayload p{};
    uint8_t lazy = 0;
    if (!r.read_u64(p.clip_id) ||
        !r.read_string(p.mime_type) ||
        !r.read_u64(p.total_size) ||
        !r.read_bytes(p.sha256) ||
        !r.read_u8(lazy)) {
        return std::nullopt;
    }
    p.lazy_pull = (lazy != 0);
    return p;
}

// --- ClipDataChunkPayload ---

std::vector<uint8_t> ClipDataChunkPayload::encode() const {
    ByteWriter w;
    w.write_u64(clip_id);
    w.write_u64(offset);
    w.write_blob(data);
    return w.take();
}

std::optional<ClipDataChunkPayload> ClipDataChunkPayload::decode(std::span<const uint8_t> data) {
    ByteReader r(data);
    ClipDataChunkPayload p{};
    if (!r.read_u64(p.clip_id) ||
        !r.read_u64(p.offset) ||
        !r.read_blob(p.data, kMaxChunkSize)) {
        return std::nullopt;
    }
    return p;
}

// --- ClipPullReqPayload ---

std::vector<uint8_t> ClipPullReqPayload::encode() const {
    ByteWriter w;
    w.write_u64(clip_id);
    return w.take();
}

std::optional<ClipPullReqPayload> ClipPullReqPayload::decode(std::span<const uint8_t> data) {
    ByteReader r(data);
    ClipPullReqPayload p{};
    if (!r.read_u64(p.clip_id)) {
        return std::nullopt;
    }
    return p;
}

// --- FileBatchMetaPayload ---

std::vector<uint8_t> FileBatchMetaPayload::encode() const {
    ByteWriter w;
    w.write_u64(batch_id);
    w.write_u32(total_files);
    w.write_u64(total_bytes);
    w.write_u32(static_cast<uint32_t>(files.size()));
    for (const auto& f : files) {
        w.write_u32(f.file_index);
        w.write_string(f.relative_path);
        w.write_u64(f.file_size);
        w.write_u64(f.last_modified_ms);
        w.write_bytes(f.file_sha256);
    }
    return w.take();
}

std::optional<FileBatchMetaPayload> FileBatchMetaPayload::decode(std::span<const uint8_t> data) {
    ByteReader r(data);
    FileBatchMetaPayload p{};
    uint32_t count = 0;
    if (!r.read_u64(p.batch_id) ||
        !r.read_u32(p.total_files) ||
        !r.read_u64(p.total_bytes) ||
        !r.read_u32(count)) {
        return std::nullopt;
    }
    constexpr size_t kMinFileMetaEntrySize = 54;
    if (count > 100000 || count > r.remaining() / kMinFileMetaEntrySize) {
        return std::nullopt;
    }
    p.files.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        FileEntryMeta e{};
        if (!r.read_u32(e.file_index) ||
            !r.read_string(e.relative_path) ||
            !r.read_u64(e.file_size) ||
            !r.read_u64(e.last_modified_ms) ||
            !r.read_bytes(e.file_sha256)) {
            return std::nullopt;
        }
        p.files.push_back(std::move(e));
    }
    return p;
}

// --- FileResumeOffsetPayload ---

std::vector<uint8_t> FileResumeOffsetPayload::encode() const {
    ByteWriter w;
    w.write_u64(batch_id);
    w.write_u32(static_cast<uint32_t>(entries.size()));
    for (const auto& e : entries) {
        w.write_u32(e.file_index);
        w.write_u64(e.resume_offset);
        w.write_bytes(e.tail_block_sha256);
    }
    return w.take();
}

std::optional<FileResumeOffsetPayload> FileResumeOffsetPayload::decode(std::span<const uint8_t> data) {
    ByteReader r(data);
    FileResumeOffsetPayload p{};
    uint32_t count = 0;
    if (!r.read_u64(p.batch_id) || !r.read_u32(count)) {
        return std::nullopt;
    }
    constexpr size_t kMinResumeEntrySize = 44;
    if (count > 100000 || count > r.remaining() / kMinResumeEntrySize) {
        return std::nullopt;
    }
    p.entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        FileResumeEntry e{};
        if (!r.read_u32(e.file_index) ||
            !r.read_u64(e.resume_offset) ||
            !r.read_bytes(e.tail_block_sha256)) {
            return std::nullopt;
        }
        p.entries.push_back(e);
    }
    return p;
}

// --- FileDataChunkPayload ---

std::vector<uint8_t> FileDataChunkPayload::encode() const {
    ByteWriter w;
    w.write_u64(batch_id);
    w.write_u32(file_index);
    w.write_u64(offset);
    w.write_blob(data);
    return w.take();
}

std::optional<FileDataChunkPayload> FileDataChunkPayload::decode(std::span<const uint8_t> data) {
    ByteReader r(data);
    FileDataChunkPayload p{};
    if (!r.read_u64(p.batch_id) ||
        !r.read_u32(p.file_index) ||
        !r.read_u64(p.offset) ||
        !r.read_blob(p.data, kMaxChunkSize)) {
        return std::nullopt;
    }
    return p;
}

// --- FileVerifiedAckPayload ---

std::vector<uint8_t> FileVerifiedAckPayload::encode() const {
    ByteWriter w;
    w.write_u64(batch_id);
    w.write_u32(file_index);
    w.write_u8(success ? 1 : 0);
    w.write_string(error_message);
    return w.take();
}

std::optional<FileVerifiedAckPayload> FileVerifiedAckPayload::decode(std::span<const uint8_t> data) {
    ByteReader r(data);
    FileVerifiedAckPayload p{};
    uint8_t ok = 0;
    if (!r.read_u64(p.batch_id) ||
        !r.read_u32(p.file_index) ||
        !r.read_u8(ok) ||
        !r.read_string(p.error_message)) {
        return std::nullopt;
    }
    p.success = (ok != 0);
    return p;
}

}  // namespace l2a
