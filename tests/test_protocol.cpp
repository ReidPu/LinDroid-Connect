#include "l2a/protocol.hpp"
#include "l2a/sha256.hpp"
#include "l2a/tls_channel.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
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

void test_sha256_and_crypto() {
    auto d = sha256_string("abc");
    std::string hex = to_hex(d);
    TEST_CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    Key256 alice_priv{};
    Key256 bob_priv{};
    for (size_t i = 0; i < 32; ++i) {
        alice_priv[i] = static_cast<uint8_t>(i + 1);
        bob_priv[i] = static_cast<uint8_t>(100 + i);
    }
    Key256 alice_pub = x25519_basepoint(alice_priv);
    Key256 bob_pub = x25519_basepoint(bob_priv);

    Key256 shared_ab = x25519_shared(alice_priv, bob_pub);
    Key256 shared_ba = x25519_shared(bob_priv, alice_pub);
    TEST_CHECK(shared_ab == shared_ba);

    Nonce96 nonce{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::vector<uint8_t> aad = {0x4C, 0x32, 0x01, 0x10};
    std::string secret_msg = "Hello from Redmi G Pro 2024 NFC Clipboard!";
    std::span<const uint8_t> pt(
        reinterpret_cast<const uint8_t*>(secret_msg.data()), secret_msg.size());

    auto ct = chacha20_poly1305_encrypt(shared_ab, nonce, aad, pt);
    TEST_CHECK(ct.size() == pt.size() + 16);

    auto dec = chacha20_poly1305_decrypt(shared_ba, nonce, aad, ct);
    TEST_CHECK(dec.has_value());
    std::string recovered(reinterpret_cast<const char*>(dec->data()), dec->size());
    TEST_CHECK(recovered == secret_msg);

    ct[0] ^= 0x01;
    auto tampered = chacha20_poly1305_decrypt(shared_ba, nonce, aad, ct);
    TEST_CHECK(!tampered.has_value());

    std::cout << "[PASS] SHA-256, X25519 ECDH, and ChaCha20-Poly1305 AEAD verified.\n";
}

void test_binary_framing_and_payloads() {
    ClipOfferPayload offer{};
    offer.clip_id = 42;
    offer.mime_type = "image/png";
    offer.total_size = 196608;
    offer.sha256 = sha256_string("dummy_image_payload");
    offer.lazy_pull = false;

    Frame f{};
    f.header.type = FrameType::kClipOffer;
    f.header.flags = FrameFlags::kAckRequired;
    f.header.stream_id = 102;
    f.payload = offer.encode();

    auto wire = f.serialize();
    TEST_CHECK(wire.size() == kFrameHeaderSize + f.payload.size());
    TEST_CHECK(wire[0] == 0x4C && wire[1] == 0x32);

    auto decoded_frame = Frame::deserialize(wire);
    TEST_CHECK(decoded_frame.has_value());
    TEST_CHECK(decoded_frame->header.type == FrameType::kClipOffer);
    TEST_CHECK(decoded_frame->header.flags == FrameFlags::kAckRequired);
    TEST_CHECK(decoded_frame->header.stream_id == 102);

    auto decoded_offer = ClipOfferPayload::decode(decoded_frame->payload);
    TEST_CHECK(decoded_offer.has_value());
    TEST_CHECK(decoded_offer->clip_id == 42);
    TEST_CHECK(decoded_offer->mime_type == "image/png");
    TEST_CHECK(decoded_offer->total_size == 196608);
    TEST_CHECK(decoded_offer->sha256 == offer.sha256);

    wire[7] ^= 0xFF;
    TEST_CHECK(!Frame::deserialize(wire).has_value());

    FileBatchMetaPayload batch{};
    batch.batch_id = 9001;
    batch.total_files = 1;
    batch.total_bytes = 1048576;
    FileEntryMeta entry{};
    entry.file_index = 0;
    entry.relative_path = "photos/IMG_2026.jpg";
    entry.file_size = 1048576;
    entry.last_modified_ms = 1760000000000ULL;
    entry.file_sha256 = sha256_string("img_content");
    batch.files.push_back(entry);

    auto batch_bytes = batch.encode();
    auto batch_dec = FileBatchMetaPayload::decode(batch_bytes);
    TEST_CHECK(batch_dec.has_value());
    TEST_CHECK(batch_dec->batch_id == 9001);
    TEST_CHECK(batch_dec->files.size() == 1);
    TEST_CHECK(batch_dec->files[0].relative_path == "photos/IMG_2026.jpg");

    std::cout << "[PASS] L2A Protocol v1 16-byte header, CRC16, and payload serialization verified.\n";
}

}  // namespace

int main() {
    test_sha256_and_crypto();
    test_binary_framing_and_payloads();
    std::cout << "All protocol & crypto unit tests passed!\n";
    return 0;
}
