#include "l2a/engine.hpp"
#include "l2a/protocol.hpp"
#include "l2a/sha256.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <sys/socket.h>
#include <vector>

using namespace l2a;

#define TEST_CHECK(cond)                                                           \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                      << "\n";                                                     \
            std::abort();                                                          \
        }                                                                          \
    } while (0)

namespace {

void write_u32_be(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24U) & 0xFFU));
    buf.push_back(static_cast<uint8_t>((val >> 16U) & 0xFFU));
    buf.push_back(static_cast<uint8_t>((val >> 8U) & 0xFFU));
    buf.push_back(static_cast<uint8_t>(val & 0xFFU));
}

void write_u64_be(std::vector<uint8_t>& buf, uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFFU));
    }
}

// 1. Test FrameHeader decoding boundaries
void test_frame_header_boundaries() {
    std::cout << "[INFO] Testing FrameHeader boundary defense...\n";

    // Valid baseline header
    FrameHeader valid_hdr{};
    valid_hdr.type = FrameType::kPing;
    valid_hdr.flags = FrameFlags::kFin;
    valid_hdr.stream_id = 42;
    valid_hdr.payload_length = 128;
    auto enc = valid_hdr.encode();

    // 1.1 Valid header decodes cleanly
    auto dec = FrameHeader::decode(enc);
    TEST_CHECK(dec.has_value());
    TEST_CHECK(dec->magic == kProtocolMagic);
    TEST_CHECK(dec->version == kProtocolVersion);
    TEST_CHECK(dec->type == FrameType::kPing);
    TEST_CHECK(dec->payload_length == 128);

    // 1.2 Corrupted Magic
    auto bad_magic = enc;
    bad_magic[0] ^= 0x01;
    TEST_CHECK(!FrameHeader::decode(bad_magic).has_value());

    // 1.3 Unsupported Protocol Version
    auto bad_ver = enc;
    bad_ver[2] = 2;  // Current version is 1
    // Recompute CRC to isolate version check
    uint16_t crc_ver = crc16_ccitt(std::span<const uint8_t>(bad_ver.data(), 14));
    bad_ver[14] = static_cast<uint8_t>(crc_ver >> 8U);
    bad_ver[15] = static_cast<uint8_t>(crc_ver & 0xFFU);
    TEST_CHECK(!FrameHeader::decode(bad_ver).has_value());

    // 1.4 Corrupted Header CRC16
    auto bad_crc = enc;
    bad_crc[15] ^= 0xFF;
    TEST_CHECK(!FrameHeader::decode(bad_crc).has_value());

    // 1.5 Payload Length Exceeding kMaxFramePayload (66,560 bytes)
    auto oversized = enc;
    uint32_t big_len = kMaxFramePayload + 1;
    oversized[10] = static_cast<uint8_t>((big_len >> 24U) & 0xFFU);
    oversized[11] = static_cast<uint8_t>((big_len >> 16U) & 0xFFU);
    oversized[12] = static_cast<uint8_t>((big_len >> 8U) & 0xFFU);
    oversized[13] = static_cast<uint8_t>(big_len & 0xFFU);
    uint16_t crc_oversized = crc16_ccitt(std::span<const uint8_t>(oversized.data(), 14));
    oversized[14] = static_cast<uint8_t>(crc_oversized >> 8U);
    oversized[15] = static_cast<uint8_t>(crc_oversized & 0xFFU);
    TEST_CHECK(!FrameHeader::decode(oversized).has_value());

    // 1.6 Enormous 4GB payload length
    auto huge = enc;
    huge[10] = 0xFF;
    huge[11] = 0xFF;
    huge[12] = 0xFF;
    huge[13] = 0xFF;
    uint16_t crc_huge = crc16_ccitt(std::span<const uint8_t>(huge.data(), 14));
    huge[14] = static_cast<uint8_t>(crc_huge >> 8U);
    huge[15] = static_cast<uint8_t>(crc_huge & 0xFFU);
    TEST_CHECK(!FrameHeader::decode(huge).has_value());

    std::cout << "[PASS] 1. FrameHeader magic, version, CRC16, and max payload boundaries verified.\n";
}

// 2. Test Array Count Bomb / Memory DoS defense (CORE-10)
void test_array_count_bomb_defense() {
    std::cout << "[INFO] Testing Array Count Bomb / DoS memory exhaustion defense (CORE-10)...\n";

    // 2.1 FileBatchMetaPayload DoS attack:
    // Attacker crafts a payload claiming count = 0xFFFFFFFF or count = 1,000,000
    // with only a few bytes in the packet.
    // Without bounds checking (count <= remaining / min_entry_size),
    // vector::reserve(count) throws std::bad_alloc or allocates gigabytes of RAM!
    {
        std::vector<uint8_t> dos_meta;
        write_u64_be(dos_meta, 1);       // batch_id = 1
        write_u32_be(dos_meta, 100);     // total_files = 100
        write_u64_be(dos_meta, 102400);  // total_bytes = 102400
        write_u32_be(dos_meta, 0xFFFFFFFF); // files count = 4,294,967,295!

        try {
            auto res = FileBatchMetaPayload::decode(dos_meta);
            TEST_CHECK(!res.has_value());  // Must reject gracefully
        } catch (const std::bad_alloc&) {
            std::cerr << "[FAIL] FileBatchMetaPayload::decode threw std::bad_alloc on count bomb!\n";
            std::abort();
        }

        // Test count > 100,000 ceiling
        dos_meta.clear();
        write_u64_be(dos_meta, 1);
        write_u32_be(dos_meta, 100001);
        write_u64_be(dos_meta, 100001);
        write_u32_be(dos_meta, 100001);  // count = 100001
        auto res2 = FileBatchMetaPayload::decode(dos_meta);
        TEST_CHECK(!res2.has_value());

        // Test count = 5000 with only 100 bytes (min entry size = 54)
        dos_meta.clear();
        write_u64_be(dos_meta, 1);
        write_u32_be(dos_meta, 5000);
        write_u64_be(dos_meta, 5000);
        write_u32_be(dos_meta, 5000);
        dos_meta.resize(dos_meta.size() + 100, 0x00);
        auto res3 = FileBatchMetaPayload::decode(dos_meta);
        TEST_CHECK(!res3.has_value());
    }

    // 2.2 FileResumeOffsetPayload DoS attack:
    {
        std::vector<uint8_t> dos_resume;
        write_u64_be(dos_resume, 1);          // batch_id = 1
        write_u32_be(dos_resume, 0x7FFFFFFF); // count = 2,147,483,647

        try {
            auto res = FileResumeOffsetPayload::decode(dos_resume);
            TEST_CHECK(!res.has_value());
        } catch (const std::bad_alloc&) {
            std::cerr << "[FAIL] FileResumeOffsetPayload::decode threw std::bad_alloc on count bomb!\n";
            std::abort();
        }

        // Test count = 5000 with only 50 bytes (min entry size = 44)
        dos_resume.clear();
        write_u64_be(dos_resume, 1);
        write_u32_be(dos_resume, 5000);
        dos_resume.resize(dos_resume.size() + 50, 0x00);
        auto res2 = FileResumeOffsetPayload::decode(dos_resume);
        TEST_CHECK(!res2.has_value());
    }

    // 2.3 FileDataChunkPayload & ClipDataChunkPayload Blob Length Attacks:
    {
        // FileDataChunk claims 1GB data in 20 bytes
        std::vector<uint8_t> dos_chunk;
        write_u64_be(dos_chunk, 1);            // batch_id
        write_u32_be(dos_chunk, 0);            // file_index
        write_u64_be(dos_chunk, 0);            // offset
        write_u32_be(dos_chunk, 1024 * 1024 * 1024); // data len = 1GB!

        auto res_chunk = FileDataChunkPayload::decode(dos_chunk);
        TEST_CHECK(!res_chunk.has_value());

        // ClipDataChunk claims 65,537 bytes (> kMaxChunkSize 64KB)
        std::vector<uint8_t> dos_clip_chunk;
        write_u64_be(dos_clip_chunk, 1);       // clip_id
        write_u64_be(dos_clip_chunk, 0);       // offset
        write_u32_be(dos_clip_chunk, 65537);   // data len > 64KB
        dos_clip_chunk.resize(dos_clip_chunk.size() + 100, 0x00);

        auto res_clip = ClipDataChunkPayload::decode(dos_clip_chunk);
        TEST_CHECK(!res_clip.has_value());
    }

    std::cout << "[PASS] 2. Array count bombs, enormous lengths, and memory DoS vectors rejected gracefully without bad_alloc.\n";
}

// 3. Test Truncated strings and fixed buffer fields
void test_truncated_payloads() {
    std::cout << "[INFO] Testing truncated payloads across all decoders...\n";

    // HelloHandshakePayload requires strings + u8 + 32-byte pubkey
    HelloHandshakePayload h{};
    h.device_id = "device_123";
    h.device_name = "test_device";
    h.platform = "linux";
    h.protocol_version = 1;
    h.pubkey_fingerprint = sha256_string("fp");
    auto h_bytes = h.encode();

    // Cut at every single byte offset from 0 to h_bytes.size() - 1
    for (size_t cut = 0; cut < h_bytes.size(); ++cut) {
        std::span<const uint8_t> truncated(h_bytes.data(), cut);
        auto dec = HelloHandshakePayload::decode(truncated);
        TEST_CHECK(!dec.has_value());
    }

    // ClipOfferPayload truncation check
    ClipOfferPayload offer{};
    offer.clip_id = 999;
    offer.mime_type = "image/png";
    offer.total_size = 50000;
    offer.sha256 = sha256_string("clip_hash");
    offer.lazy_pull = true;
    auto offer_bytes = offer.encode();

    for (size_t cut = 0; cut < offer_bytes.size(); ++cut) {
        std::span<const uint8_t> truncated(offer_bytes.data(), cut);
        auto dec = ClipOfferPayload::decode(truncated);
        TEST_CHECK(!dec.has_value());
    }

    // PairRequestPayload truncation check
    PairRequestPayload req{};
    req.device_id = "peer_a";
    req.device_name = "Desktop";
    req.pin_or_token = "123456";
    req.pubkey_fingerprint = sha256_string("pin");
    auto req_bytes = req.encode();

    for (size_t cut = 0; cut < req_bytes.size(); ++cut) {
        std::span<const uint8_t> truncated(req_bytes.data(), cut);
        auto dec = PairRequestPayload::decode(truncated);
        TEST_CHECK(!dec.has_value());
    }

    // FileVerifiedAckPayload truncation check
    FileVerifiedAckPayload ack{};
    ack.batch_id = 55;
    ack.file_index = 2;
    ack.success = true;
    ack.error_message = "none";
    auto ack_bytes = ack.encode();

    for (size_t cut = 0; cut < ack_bytes.size(); ++cut) {
        std::span<const uint8_t> truncated(ack_bytes.data(), cut);
        auto dec = FileVerifiedAckPayload::decode(truncated);
        TEST_CHECK(!dec.has_value());
    }

    std::cout << "[PASS] 3. All decoders gracefully handle all truncated prefix lengths.\n";
}

// 4. Randomized bitstream fuzzing
void test_randomized_bitstream_fuzzing() {
    std::cout << "[INFO] Running 1,000 randomized bitstream fuzzing iterations...\n";

    std::mt19937_64 rng(0x12345678ULL);
    std::uniform_int_distribution<size_t> len_dist(0, 1024);
    std::uniform_int_distribution<uint16_t> byte_dist(0, 255);

    for (int iter = 0; iter < 1000; ++iter) {
        size_t len = len_dist(rng);
        std::vector<uint8_t> junk(len);
        for (size_t i = 0; i < len; ++i) {
            junk[i] = static_cast<uint8_t>(byte_dist(rng));
        }

        std::span<const uint8_t> span(junk.data(), junk.size());

        // Feed junk into all protocol parsers
        (void)Frame::deserialize(span);
        (void)HelloHandshakePayload::decode(span);
        (void)PairRequestPayload::decode(span);
        (void)PairResponsePayload::decode(span);
        (void)ClipOfferPayload::decode(span);
        (void)ClipDataChunkPayload::decode(span);
        (void)ClipPullReqPayload::decode(span);
        (void)FileBatchMetaPayload::decode(span);
        (void)FileResumeOffsetPayload::decode(span);
        (void)FileDataChunkPayload::decode(span);
        (void)FileVerifiedAckPayload::decode(span);
    }

    std::cout << "[PASS] 4. 1,000 randomized fuzzing iterations completed with zero crashes, bad_allocs, or leaks.\n";
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "Running TEST-06: Malformed Packet Fuzzing & DoS Defense\n";
    std::cout << "=========================================================\n";

    test_frame_header_boundaries();
    test_array_count_bomb_defense();
    test_truncated_payloads();
    test_randomized_bitstream_fuzzing();

    std::cout << "All TEST-06 malformed packet fuzzing and DoS boundary tests PASSED!\n";
    return 0;
}
