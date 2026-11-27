#include "l2a/tls_channel.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace l2a {

namespace {

void fill_random_bytes(std::span<uint8_t> out) {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.is_open()) {
        urandom.read(reinterpret_cast<char*>(out.data()),
                     static_cast<std::streamsize>(out.size()));
        if (urandom.gcount() == static_cast<std::streamsize>(out.size())) {
            return;
        }
    }
    std::random_device rd;
    for (auto& b : out) {
        b = static_cast<uint8_t>(rd() & 0xFFU);
    }
}

// --- RFC 7748 X25519 Field Arithmetic (GF(2^255 - 19)) ---

using Fe = std::array<int64_t, 16>;

void fe_carry(Fe& o) noexcept {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

void fe_add(Fe& o, const Fe& a, const Fe& b) noexcept {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

void fe_sub(Fe& o, const Fe& a, const Fe& b) noexcept {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

void fe_mul(Fe& o, const Fe& a, const Fe& b) noexcept {
    int64_t t[31]{};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; ++i) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    fe_carry(o);
    fe_carry(o);
}

void fe_cswap(Fe& p, Fe& q, int64_t bit) noexcept {
    int64_t mask = ~(bit - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void fe_unpack(Fe& o, const Key256& n) noexcept {
    for (int i = 0; i < 16; ++i) {
        o[i] = static_cast<int64_t>(n[2 * i]) +
               (static_cast<int64_t>(n[2 * i + 1]) << 8);
    }
    o[15] &= 0x7FFF;
}

Key256 fe_pack(const Fe& n) noexcept {
    Fe t = n;
    fe_carry(t);
    fe_carry(t);
    fe_carry(t);
    Fe m{};
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xFFED;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xFFFF - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xFFFF;
        }
        m[15] = t[15] - 0x7FFF - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xFFFF;
        fe_cswap(t, m, 1 - b);
    }
    Key256 out{};
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = static_cast<uint8_t>(t[i] & 0xFF);
        out[2 * i + 1] = static_cast<uint8_t>((t[i] >> 8) & 0xFF);
    }
    return out;
}

void fe_inv(Fe& o, const Fe& i) noexcept {
    Fe c = i;
    for (int a = 253; a >= 0; --a) {
        fe_mul(c, c, c);
        if (a != 2 && a != 4) {
            fe_mul(c, c, i);
        }
    }
    o = c;
}

// --- RFC 8439 ChaCha20 & Poly1305 ---

constexpr uint32_t rotl32(uint32_t x, int n) noexcept {
    return (x << n) | (x >> (32 - n));
}

uint32_t load_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}

void store_le32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
}

void store_le64(uint8_t* p, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFFU);
    }
}

void chacha20_quarter_round(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) noexcept {
    a += b; d ^= a; d = rotl32(d, 16);
    c += d; b ^= c; b = rotl32(b, 12);
    a += b; d ^= a; d = rotl32(d, 8);
    c += d; b ^= c; b = rotl32(b, 7);
}

void chacha20_block(
    const Key256& key,
    uint32_t counter,
    const Nonce96& nonce,
    uint8_t out[64]) noexcept {
    uint32_t state[16] = {
        0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U,
        load_le32(&key[0]),  load_le32(&key[4]),  load_le32(&key[8]),  load_le32(&key[12]),
        load_le32(&key[16]), load_le32(&key[20]), load_le32(&key[24]), load_le32(&key[28]),
        counter,
        load_le32(&nonce[0]), load_le32(&nonce[4]), load_le32(&nonce[8])
    };

    uint32_t w[16];
    std::memcpy(w, state, sizeof(state));

    for (int i = 0; i < 10; ++i) {
        chacha20_quarter_round(w[0], w[4], w[8],  w[12]);
        chacha20_quarter_round(w[1], w[5], w[9],  w[13]);
        chacha20_quarter_round(w[2], w[6], w[10], w[14]);
        chacha20_quarter_round(w[3], w[7], w[11], w[15]);
        chacha20_quarter_round(w[0], w[5], w[10], w[15]);
        chacha20_quarter_round(w[1], w[6], w[11], w[12]);
        chacha20_quarter_round(w[2], w[7], w[8],  w[13]);
        chacha20_quarter_round(w[3], w[4], w[9],  w[14]);
    }

    for (int i = 0; i < 16; ++i) {
        store_le32(out + i * 4, w[i] + state[i]);
    }
}

void chacha20_xor(
    const Key256& key,
    uint32_t initial_counter,
    const Nonce96& nonce,
    std::span<const uint8_t> input,
    uint8_t* output) noexcept {
    uint8_t block[64];
    uint32_t counter = initial_counter;
    size_t offset = 0;
    while (offset < input.size()) {
        chacha20_block(key, counter++, nonce, block);
        size_t chunk = std::min<size_t>(64, input.size() - offset);
        for (size_t i = 0; i < chunk; ++i) {
            output[offset + i] = input[offset + i] ^ block[i];
        }
        offset += chunk;
    }
}

// RFC 8439 Poly1305 MAC (using 26-bit limbs in 64-bit integers)
Tag128 poly1305_mac(const uint8_t key[32], std::span<const uint8_t> msg) noexcept {
    uint32_t r0 = load_le32(key + 0) & 0x3ffffffU;
    uint32_t r1 = (load_le32(key + 3) >> 2U) & 0x3ffff03U;
    uint32_t r2 = (load_le32(key + 6) >> 4U) & 0x3ffc0ffU;
    uint32_t r3 = (load_le32(key + 9) >> 6U) & 0x3f03fffU;
    uint32_t r4 = (load_le32(key + 12) >> 8U) & 0x00fffffU;

    uint32_t s1 = r1 * 5U;
    uint32_t s2 = r2 * 5U;
    uint32_t s3 = r3 * 5U;
    uint32_t s4 = r4 * 5U;

    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    size_t offset = 0;
    while (offset < msg.size()) {
        uint8_t block[17]{};
        size_t chunk = std::min<size_t>(16, msg.size() - offset);
        std::memcpy(block, msg.data() + offset, chunk);
        block[chunk] = 1;  // hibit

        h0 += load_le32(block + 0) & 0x3ffffffU;
        h1 += (load_le32(block + 3) >> 2U) & 0x3ffffffU;
        h2 += (load_le32(block + 6) >> 4U) & 0x3ffffffU;
        h3 += (load_le32(block + 9) >> 6U) & 0x3ffffffU;
        h4 += (load_le32(block + 12) >> 8U) | (static_cast<uint32_t>(block[16]) << 24U);

        uint64_t d0 = static_cast<uint64_t>(h0) * r0 + static_cast<uint64_t>(h1) * s4 +
                      static_cast<uint64_t>(h2) * s3 + static_cast<uint64_t>(h3) * s2 +
                      static_cast<uint64_t>(h4) * s1;
        uint64_t d1 = static_cast<uint64_t>(h0) * r1 + static_cast<uint64_t>(h1) * r0 +
                      static_cast<uint64_t>(h2) * s4 + static_cast<uint64_t>(h3) * s3 +
                      static_cast<uint64_t>(h4) * s2;
        uint64_t d2 = static_cast<uint64_t>(h0) * r2 + static_cast<uint64_t>(h1) * r1 +
                      static_cast<uint64_t>(h2) * r0 + static_cast<uint64_t>(h3) * s4 +
                      static_cast<uint64_t>(h4) * s3;
        uint64_t d3 = static_cast<uint64_t>(h0) * r3 + static_cast<uint64_t>(h1) * r2 +
                      static_cast<uint64_t>(h2) * r1 + static_cast<uint64_t>(h3) * r0 +
                      static_cast<uint64_t>(h4) * s4;
        uint64_t d4 = static_cast<uint64_t>(h0) * r4 + static_cast<uint64_t>(h1) * r3 +
                      static_cast<uint64_t>(h2) * r2 + static_cast<uint64_t>(h3) * r1 +
                      static_cast<uint64_t>(h4) * r0;

        uint32_t c = static_cast<uint32_t>(d0 >> 26U); h0 = static_cast<uint32_t>(d0) & 0x3ffffffU;
        d1 += c; c = static_cast<uint32_t>(d1 >> 26U); h1 = static_cast<uint32_t>(d1) & 0x3ffffffU;
        d2 += c; c = static_cast<uint32_t>(d2 >> 26U); h2 = static_cast<uint32_t>(d2) & 0x3ffffffU;
        d3 += c; c = static_cast<uint32_t>(d3 >> 26U); h3 = static_cast<uint32_t>(d3) & 0x3ffffffU;
        d4 += c; c = static_cast<uint32_t>(d4 >> 26U); h4 = static_cast<uint32_t>(d4) & 0x3ffffffU;
        h0 += c * 5U; c = h0 >> 26U; h0 &= 0x3ffffffU;
        h1 += c;

        offset += chunk;
    }

    uint32_t c = h1 >> 26U; h1 &= 0x3ffffffU;
    h2 += c; c = h2 >> 26U; h2 &= 0x3ffffffU;
    h3 += c; c = h3 >> 26U; h3 &= 0x3ffffffU;
    h4 += c; c = h4 >> 26U; h4 &= 0x3ffffffU;
    h0 += c * 5U; c = h0 >> 26U; h0 &= 0x3ffffffU;
    h1 += c;

    uint32_t g0 = h0 + 5U; c = g0 >> 26U; g0 &= 0x3ffffffU;
    uint32_t g1 = h1 + c;  c = g1 >> 26U; g1 &= 0x3ffffffU;
    uint32_t g2 = h2 + c;  c = g2 >> 26U; g2 &= 0x3ffffffU;
    uint32_t g3 = h3 + c;  c = g3 >> 26U; g3 &= 0x3ffffffU;
    uint32_t g4 = h4 + c - (1U << 26U);

    uint32_t mask = (g4 >> 31U) - 1U;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    uint64_t f0 = ((h0) | (h1 << 26U)) + static_cast<uint64_t>(load_le32(key + 16));
    uint64_t f1 = ((h1 >> 6U) | (h2 << 20U)) + static_cast<uint64_t>(load_le32(key + 20));
    uint64_t f2 = ((h2 >> 12U) | (h3 << 14U)) + static_cast<uint64_t>(load_le32(key + 24));
    uint64_t f3 = ((h3 >> 18U) | (h4 << 8U)) + static_cast<uint64_t>(load_le32(key + 28));

    Tag128 tag{};
    store_le32(&tag[0], static_cast<uint32_t>(f0)); f1 += (f0 >> 32U);
    store_le32(&tag[4], static_cast<uint32_t>(f1)); f2 += (f1 >> 32U);
    store_le32(&tag[8], static_cast<uint32_t>(f2)); f3 += (f2 >> 32U);
    store_le32(&tag[12], static_cast<uint32_t>(f3));
    return tag;
}

Tag128 compute_aead_tag(
    const Key256& key,
    const Nonce96& nonce,
    std::span<const uint8_t> aad,
    std::span<const uint8_t> ciphertext) noexcept {
    uint8_t poly_key[64]{};
    chacha20_block(key, 0, nonce, poly_key);

    std::vector<uint8_t> mac_data;
    size_t aad_pad = (16 - (aad.size() % 16)) % 16;
    size_t ct_pad = (16 - (ciphertext.size() % 16)) % 16;
    mac_data.reserve(aad.size() + aad_pad + ciphertext.size() + ct_pad + 16);

    mac_data.insert(mac_data.end(), aad.begin(), aad.end());
    mac_data.insert(mac_data.end(), aad_pad, 0);
    mac_data.insert(mac_data.end(), ciphertext.begin(), ciphertext.end());
    mac_data.insert(mac_data.end(), ct_pad, 0);

    uint8_t lengths[16]{};
    store_le64(lengths, aad.size());
    store_le64(lengths + 8, ciphertext.size());
    mac_data.insert(mac_data.end(), lengths, lengths + 16);

    return poly1305_mac(poly_key, mac_data);
}

}  // namespace

Key256 x25519_shared(const Key256& scalar, const Key256& peer_public) noexcept {
    Key256 z = scalar;
    z[0] &= 248;
    z[31] = (z[31] & 127) | 64;

    Fe x{}, a{}, b{}, c{}, d{}, e{}, f{};
    fe_unpack(x, peer_public);
    a[0] = 1;
    d[0] = 1;
    b = x;

    static constexpr Fe k121665 = {0xDB41, 1};

    for (int i = 254; i >= 0; --i) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        fe_cswap(a, b, r);
        fe_cswap(c, d, r);
        fe_add(e, a, c);
        fe_sub(a, a, c);
        fe_add(c, b, d);
        fe_sub(b, b, d);
        fe_mul(d, e, e);
        fe_mul(f, a, a);
        fe_mul(a, c, a);
        fe_mul(c, b, e);
        fe_add(e, a, c);
        fe_sub(a, a, c);
        fe_mul(b, a, a);
        fe_sub(c, d, f);
        fe_mul(a, c, k121665);
        fe_add(a, a, d);
        fe_mul(c, c, a);
        fe_mul(a, d, f);
        fe_mul(d, b, x);
        fe_mul(b, e, e);
        fe_cswap(a, b, r);
        fe_cswap(c, d, r);
    }

    fe_inv(c, c);
    fe_mul(a, a, c);
    return fe_pack(a);
}

Key256 x25519_basepoint(const Key256& scalar) noexcept {
    Key256 base{};
    base[0] = 9;
    return x25519_shared(scalar, base);
}

std::vector<uint8_t> chacha20_poly1305_encrypt(
    const Key256& key,
    const Nonce96& nonce,
    std::span<const uint8_t> aad,
    std::span<const uint8_t> plaintext) {
    std::vector<uint8_t> out(plaintext.size() + 16);
    chacha20_xor(key, 1, nonce, plaintext, out.data());
    Tag128 tag = compute_aead_tag(
        key, nonce, aad, std::span<const uint8_t>(out.data(), plaintext.size()));
    std::memcpy(out.data() + plaintext.size(), tag.data(), 16);
    return out;
}

std::optional<std::vector<uint8_t>> chacha20_poly1305_decrypt(
    const Key256& key,
    const Nonce96& nonce,
    std::span<const uint8_t> aad,
    std::span<const uint8_t> ciphertext_and_tag) {
    if (ciphertext_and_tag.size() < 16) {
        return std::nullopt;
    }
    size_t ct_len = ciphertext_and_tag.size() - 16;
    std::span<const uint8_t> ct(ciphertext_and_tag.data(), ct_len);
    const uint8_t* received_tag = ciphertext_and_tag.data() + ct_len;

    Tag128 expected_tag = compute_aead_tag(key, nonce, aad, ct);
    uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i) {
        diff |= (received_tag[i] ^ expected_tag[i]);
    }
    if (diff != 0) {
        return std::nullopt;
    }

    std::vector<uint8_t> plaintext(ct_len);
    chacha20_xor(key, 1, nonce, ct, plaintext.data());
    return plaintext;
}

// --- DeviceIdentity & TrustedPeerStore ---

namespace {

std::string escape_tsv_field(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string unescape_tsv_field(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[++i];
            switch (next) {
                case '\\': out += '\\'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: out += next; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace

DeviceIdentity DeviceIdentity::load_or_create(
    const std::filesystem::path& storage_dir,
    std::string_view default_device_name) {
    std::error_code ec;
    std::filesystem::create_directories(storage_dir, ec);
    auto id_file = storage_dir / "identity.cfg";

    DeviceIdentity id{};
    if (std::filesystem::exists(id_file)) {
        std::ifstream in(id_file);
        std::string raw_id, raw_name, priv_hex;
        if (std::getline(in, raw_id) &&
            std::getline(in, raw_name) &&
            std::getline(in, priv_hex)) {
            if (!raw_id.empty() && raw_id.back() == '\r') raw_id.pop_back();
            if (!raw_name.empty() && raw_name.back() == '\r') raw_name.pop_back();
            if (!priv_hex.empty() && priv_hex.back() == '\r') priv_hex.pop_back();
            id.device_id = unescape_tsv_field(raw_id);
            id.device_name = unescape_tsv_field(raw_name);
            auto priv = from_hex(priv_hex);
            if (priv && priv->size() == 32) {
                std::memcpy(id.private_key.data(), priv->data(), 32);
                id.public_key = x25519_basepoint(id.private_key);
                id.fingerprint = sha256_bytes(id.public_key);
                return id;
            }
        }
    }

    fill_random_bytes(id.private_key);
    id.public_key = x25519_basepoint(id.private_key);
    id.fingerprint = sha256_bytes(id.public_key);
    id.device_id = "l2a-" + to_hex(std::span<const uint8_t>(id.fingerprint.data(), 8));
    id.device_name = std::string(default_device_name);

    std::ofstream out(id_file, std::ios::trunc);
    if (out.is_open()) {
        out << escape_tsv_field(id.device_id) << "\n"
            << escape_tsv_field(id.device_name) << "\n"
            << to_hex(id.private_key) << "\n";
    }
    return id;
}

TrustedPeerStore::TrustedPeerStore(std::filesystem::path store_file)
    : store_file_(std::move(store_file)) {
    load();
}

void TrustedPeerStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.clear();
    std::ifstream in(store_file_);
    if (!in.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t sep1 = line.find('\t');
        std::string dev_id, pub_hex, dev_name;
        if (sep1 != std::string::npos) {
            size_t sep2 = line.find('\t', sep1 + 1);
            if (sep2 == std::string::npos) continue;
            dev_id = unescape_tsv_field(line.substr(0, sep1));
            pub_hex = line.substr(sep1 + 1, sep2 - (sep1 + 1));
            dev_name = unescape_tsv_field(line.substr(sep2 + 1));
        } else {
            std::istringstream iss(line);
            if (!(iss >> dev_id >> pub_hex)) continue;
            std::getline(iss >> std::ws, dev_name);
            if (!dev_name.empty() && dev_name.back() == '\r') dev_name.pop_back();
            dev_id = unescape_tsv_field(dev_id);
            dev_name = unescape_tsv_field(dev_name);
        }
        auto pub_bytes = from_hex(pub_hex);
        if (!pub_bytes || pub_bytes->size() != 32) continue;

        TrustedPeer p{};
        p.device_id = dev_id;
        p.device_name = dev_name;
        std::memcpy(p.public_key.data(), pub_bytes->data(), 32);
        p.fingerprint = sha256_bytes(p.public_key);
        peers_[p.device_id] = std::move(p);
    }
}

bool TrustedPeerStore::save() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (store_file_.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(store_file_.parent_path(), ec);
    }
    std::ofstream out(store_file_, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    for (const auto& [_, peer] : peers_) {
        out << escape_tsv_field(peer.device_id) << "\t"
            << to_hex(peer.public_key) << "\t"
            << escape_tsv_field(peer.device_name) << "\n";
    }
    return true;
}

std::optional<TrustedPeer> TrustedPeerStore::find(std::string_view device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(std::string(device_id));
    if (it == peers_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void TrustedPeerStore::upsert(const TrustedPeer& peer) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peers_[peer.device_id] = peer;
    }
    save();
}

std::vector<TrustedPeer> TrustedPeerStore::list_all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TrustedPeer> out;
    out.reserve(peers_.size());
    for (const auto& [_, p] : peers_) {
        out.push_back(p);
    }
    return out;
}

// --- TlsChannel Implementation ---

TlsChannel::TlsChannel(int socket_fd, bool is_client)
    : sock_fd_(socket_fd), is_client_(is_client) {}

TlsChannel::~TlsChannel() {
    close();
}

void TlsChannel::close() {
    int fd = sock_fd_.load();
    if (fd < 0) {
        return;
    }
    ::shutdown(fd, SHUT_RDWR);

    std::scoped_lock lock(recv_mutex_, send_mutex_);
    int to_close = sock_fd_.exchange(-1);
    if (to_close >= 0) {
        ::close(to_close);
    }
}

bool TlsChannel::write_all(std::span<const uint8_t> data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int fd = sock_fd_.load();
        if (fd < 0) {
            return false;
        }
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool TlsChannel::read_exact(std::span<uint8_t> buf) {
    size_t got = 0;
    while (got < buf.size()) {
        int fd = sock_fd_.load();
        if (fd < 0) {
            return false;
        }
        ssize_t n = ::recv(fd, buf.data() + got, buf.size() - got, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

Nonce96 TlsChannel::make_nonce(const Nonce96& base_iv, uint64_t seq) const noexcept {
    Nonce96 n = base_iv;
    for (int i = 0; i < 8; ++i) {
        n[11 - i] ^= static_cast<uint8_t>((seq >> (i * 8)) & 0xFFU);
    }
    return n;
}

bool TlsChannel::perform_handshake(
    const DeviceIdentity& local_id,
    TrustedPeerStore& peer_store,
    const TofuVerifyCallback& tofu_cb) {
    Key256 eph_priv{};
    fill_random_bytes(eph_priv);
    Key256 eph_pub = x25519_basepoint(eph_priv);

    // Wire format of HandshakeHello:
    // [id_pub 32B][eph_pub 32B][dev_id_len 2B][dev_id][dev_name_len 2B][dev_name]
    auto build_hello = [&](const DeviceIdentity& id, const Key256& ep) {
        std::vector<uint8_t> msg;
        msg.insert(msg.end(), id.public_key.begin(), id.public_key.end());
        msg.insert(msg.end(), ep.begin(), ep.end());
        uint16_t id_len = static_cast<uint16_t>(id.device_id.size());
        msg.push_back(static_cast<uint8_t>((id_len >> 8U) & 0xFFU));
        msg.push_back(static_cast<uint8_t>(id_len & 0xFFU));
        msg.insert(msg.end(), id.device_id.begin(), id.device_id.end());
        uint16_t name_len = static_cast<uint16_t>(id.device_name.size());
        msg.push_back(static_cast<uint8_t>((name_len >> 8U) & 0xFFU));
        msg.push_back(static_cast<uint8_t>(name_len & 0xFFU));
        msg.insert(msg.end(), id.device_name.begin(), id.device_name.end());
        return msg;
    };

    auto send_blob = [&](std::span<const uint8_t> b) {
        uint32_t len = static_cast<uint32_t>(b.size());
        uint8_t hdr[4] = {
            static_cast<uint8_t>((len >> 24U) & 0xFFU),
            static_cast<uint8_t>((len >> 16U) & 0xFFU),
            static_cast<uint8_t>((len >> 8U) & 0xFFU),
            static_cast<uint8_t>(len & 0xFFU)
        };
        return write_all(hdr) && write_all(b);
    };

    auto recv_blob = [&]() -> std::optional<std::vector<uint8_t>> {
        uint8_t hdr[4]{};
        if (!read_exact(hdr)) return std::nullopt;
        uint32_t len = (static_cast<uint32_t>(hdr[0]) << 24U) |
                       (static_cast<uint32_t>(hdr[1]) << 16U) |
                       (static_cast<uint32_t>(hdr[2]) << 8U) |
                       static_cast<uint32_t>(hdr[3]);
        if (len < 68 || len > 4096) return std::nullopt;
        std::vector<uint8_t> buf(len);
        if (!read_exact(buf)) return std::nullopt;
        return buf;
    };

    std::vector<uint8_t> local_hello = build_hello(local_id, eph_pub);
    std::optional<std::vector<uint8_t>> peer_hello_opt;

    if (is_client_) {
        if (!send_blob(local_hello)) return false;
        peer_hello_opt = recv_blob();
    } else {
        peer_hello_opt = recv_blob();
        if (!peer_hello_opt) return false;
        if (!send_blob(local_hello)) return false;
    }
    if (!peer_hello_opt) return false;
    const auto& peer_hello = *peer_hello_opt;

    Key256 peer_id_pub{};
    Key256 peer_eph_pub{};
    std::memcpy(peer_id_pub.data(), peer_hello.data(), 32);
    std::memcpy(peer_eph_pub.data(), peer_hello.data() + 32, 32);

    size_t pos = 64;
    if (pos + 2 > peer_hello.size()) return false;
    uint16_t peer_id_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(peer_hello[pos]) << 8U) | peer_hello[pos + 1]);
    pos += 2;
    if (pos + peer_id_len + 2 > peer_hello.size()) return false;
    peer_device_id_.assign(reinterpret_cast<const char*>(peer_hello.data() + pos), peer_id_len);
    pos += peer_id_len;

    uint16_t peer_name_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(peer_hello[pos]) << 8U) | peer_hello[pos + 1]);
    pos += 2;
    if (pos + peer_name_len != peer_hello.size()) return false;
    peer_device_name_.assign(reinterpret_cast<const char*>(peer_hello.data() + pos), peer_name_len);
    peer_fingerprint_ = sha256_bytes(peer_id_pub);

    // TOFU Verification
    if (auto known = peer_store.find(peer_device_id_)) {
        if (known->fingerprint != peer_fingerprint_) {
            // Public key changed for a known device ID -> Reject MITM!
            return false;
        }
    } else {
        bool trusted = tofu_cb ? tofu_cb(peer_device_id_, peer_device_name_, peer_fingerprint_) : true;
        if (!trusted) {
            return false;
        }
        TrustedPeer tp{};
        tp.device_id = peer_device_id_;
        tp.device_name = peer_device_name_;
        tp.public_key = peer_id_pub;
        tp.fingerprint = peer_fingerprint_;
        peer_store.upsert(tp);
    }

    // Compute 3DH mutually authenticated shared secret:
    // dh1 = X25519(eph_c, eph_s)
    // dh2 = X25519(id_c, eph_s)
    // dh3 = X25519(eph_c, id_s)
    Key256 dh1 = x25519_shared(eph_priv, peer_eph_pub);
    Key256 dh2 = is_client_ ? x25519_shared(local_id.private_key, peer_eph_pub)
                            : x25519_shared(eph_priv, peer_id_pub);
    Key256 dh3 = is_client_ ? x25519_shared(eph_priv, peer_id_pub)
                            : x25519_shared(local_id.private_key, peer_eph_pub);

    std::vector<uint8_t> ikm;
    ikm.reserve(96);
    ikm.insert(ikm.end(), dh1.begin(), dh1.end());
    ikm.insert(ikm.end(), dh2.begin(), dh2.end());
    ikm.insert(ikm.end(), dh3.begin(), dh3.end());

    Sha256Hasher transcript;
    if (is_client_) {
        transcript.update(local_hello);
        transcript.update(peer_hello);
    } else {
        transcript.update(peer_hello);
        transcript.update(local_hello);
    }
    Sha256Digest thash = transcript.finalize();

    static constexpr std::string_view kInfoStr = "L2A-TLS13-CHACHA20-POLY1305";
    auto key_material = hkdf_sha256(
        thash,
        ikm,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(kInfoStr.data()), kInfoStr.size()),
        32 + 32 + 12 + 12 + 32);

    Key256 c2s_key{}, s2c_key{}, fin_key{};
    Nonce96 c2s_iv{}, s2c_iv{};
    std::memcpy(c2s_key.data(), key_material.data() + 0, 32);
    std::memcpy(s2c_key.data(), key_material.data() + 32, 32);
    std::memcpy(c2s_iv.data(), key_material.data() + 64, 12);
    std::memcpy(s2c_iv.data(), key_material.data() + 76, 12);
    std::memcpy(fin_key.data(), key_material.data() + 88, 32);

    if (is_client_) {
        send_key_ = c2s_key; recv_key_ = s2c_key;
        send_iv_ = c2s_iv;   recv_iv_ = s2c_iv;
    } else {
        send_key_ = s2c_key; recv_key_ = c2s_key;
        send_iv_ = s2c_iv;   recv_iv_ = c2s_iv;
    }

    // Exchange Finished MACs to verify key possession
    std::string my_fin_label = is_client_ ? "CLIENT_FINISHED" : "SERVER_FINISHED";
    std::string peer_fin_label = is_client_ ? "SERVER_FINISHED" : "CLIENT_FINISHED";
    Sha256Digest my_fin = hmac_sha256(
        fin_key,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(my_fin_label.data()),
                                 my_fin_label.size()));
    Sha256Digest expected_peer_fin = hmac_sha256(
        fin_key,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(peer_fin_label.data()),
                                 peer_fin_label.size()));

    Sha256Digest got_peer_fin{};
    if (is_client_) {
        if (!write_all(my_fin) || !read_exact(got_peer_fin)) return false;
    } else {
        if (!read_exact(got_peer_fin) || !write_all(my_fin)) return false;
    }

    uint8_t fin_diff = 0;
    for (size_t i = 0; i < got_peer_fin.size(); ++i) {
        fin_diff |= (got_peer_fin[i] ^ expected_peer_fin[i]);
    }
    if (fin_diff != 0) {
        return false;
    }

    send_seq_ = 0;
    recv_seq_ = 0;
    handshake_done_ = true;
    return true;
}

bool TlsChannel::send_frame(const Frame& frame) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!handshake_done_ || sock_fd_.load() < 0) {
        return false;
    }

    std::vector<uint8_t> plaintext = frame.serialize();
    uint32_t ct_len = static_cast<uint32_t>(plaintext.size() + 16);
    uint8_t record_hdr[4] = {
        static_cast<uint8_t>((ct_len >> 24U) & 0xFFU),
        static_cast<uint8_t>((ct_len >> 16U) & 0xFFU),
        static_cast<uint8_t>((ct_len >> 8U) & 0xFFU),
        static_cast<uint8_t>(ct_len & 0xFFU)
    };

    Nonce96 nonce = make_nonce(send_iv_, send_seq_++);
    std::vector<uint8_t> ciphertext = chacha20_poly1305_encrypt(
        send_key_, nonce, record_hdr, plaintext);

    return write_all(record_hdr) && write_all(ciphertext);
}

std::optional<Frame> TlsChannel::recv_frame() {
    std::lock_guard<std::mutex> lock(recv_mutex_);
    if (!handshake_done_ || sock_fd_.load() < 0) {
        return std::nullopt;
    }

    uint8_t record_hdr[4]{};
    if (!read_exact(record_hdr)) {
        return std::nullopt;
    }
    uint32_t ct_len = (static_cast<uint32_t>(record_hdr[0]) << 24U) |
                      (static_cast<uint32_t>(record_hdr[1]) << 16U) |
                      (static_cast<uint32_t>(record_hdr[2]) << 8U) |
                      static_cast<uint32_t>(record_hdr[3]);

    if (ct_len < kFrameHeaderSize + 16 || ct_len > kMaxFramePayload + kFrameHeaderSize + 16) {
        return std::nullopt;
    }

    std::vector<uint8_t> ciphertext(ct_len);
    if (!read_exact(ciphertext)) {
        return std::nullopt;
    }

    Nonce96 nonce = make_nonce(recv_iv_, recv_seq_++);
    auto plaintext = chacha20_poly1305_decrypt(recv_key_, nonce, record_hdr, ciphertext);
    if (!plaintext) {
        return std::nullopt;
    }

    return Frame::deserialize(*plaintext);
}

}  // namespace l2a
