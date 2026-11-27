#include "l2a/clipboard_stream.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace l2a {

namespace {
// Above 512 KB, stream incoming clipboard payload to a temp file instead of heap RAM
constexpr uint64_t kMaxInlineMemoryBytes = 512 * 1024;

uint64_t initial_clip_id_seed() {
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    return ((now_ms & 0xFFFFFFFFULL) << 16U) | 1ULL;
}
}  // namespace

ClipboardStreamManager::ClipboardStreamManager(
    SendFrameFunc send_frame_fn,
    std::filesystem::path temp_dir)
    : send_frame_fn_(std::move(send_frame_fn)),
      temp_dir_(std::move(temp_dir)),
      next_clip_id_(initial_clip_id_seed()) {
    std::error_code ec;
    std::filesystem::create_directories(temp_dir_, ec);
    if (!ec && std::filesystem::exists(temp_dir_, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(temp_dir_, ec)) {
            if (entry.is_regular_file(ec)) {
                auto filename = entry.path().filename().string();
                if (filename.rfind("l2a_clip_", 0) == 0 && entry.path().extension() == ".tmp") {
                    std::filesystem::remove(entry.path(), ec);
                }
            }
        }
    }
}

void ClipboardStreamManager::set_callbacks(
    ClipboardReceivedCallback on_received,
    ClipboardLazyOfferCallback on_lazy_offer,
    ClipboardPullRequestedCallback on_pull_requested) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_received_ = std::move(on_received);
    on_lazy_offer_ = std::move(on_lazy_offer);
    on_pull_requested_ = std::move(on_pull_requested);
}

void ClipboardStreamManager::on_disconnected() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, st] : incoming_streams_) {
        if (st.temp_out) {
            st.temp_out->close();
            st.temp_out.reset();
        }
    }
    incoming_streams_.clear();
}

bool ClipboardStreamManager::is_loopback_digest(const Sha256Digest& digest) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_received_hash_.has_value() && (*last_received_hash_ == digest);
}

bool ClipboardStreamManager::push_clipboard_bytes(
    std::string_view mime_type,
    std::span<const uint8_t> content,
    bool force_lazy_pull,
    bool bypass_loopback,
    bool is_pull_response) {
    Sha256Digest digest = sha256_bytes(content);
    if (!bypass_loopback && is_loopback_digest(digest)) {
        return false;  // Anti-loopback triggered: ignore!
    }

    uint64_t clip_id = next_clip_id_.fetch_add(1);
    uint32_t stream_id = next_stream_id_.fetch_add(2);
    bool is_lazy = force_lazy_pull || (mime_type == "text/uri-list");

    if (is_lazy) {
        std::lock_guard<std::mutex> lock(mutex_);
        constexpr size_t kMaxLazySources = 64;
        while (lazy_sources_.size() >= kMaxLazySources) {
            auto oldest_it = lazy_sources_.begin();
            for (auto it = lazy_sources_.begin(); it != lazy_sources_.end(); ++it) {
                if (it->first < oldest_it->first) oldest_it = it;
            }
            lazy_sources_.erase(oldest_it);
        }
        LazySource src{};
        src.clip_id = clip_id;
        src.mime_type = std::string(mime_type);
        src.inline_bytes.assign(content.begin(), content.end());
        lazy_sources_[clip_id] = std::move(src);
    }

    ClipOfferPayload offer{};
    offer.clip_id = clip_id;
    offer.mime_type = std::string(mime_type);
    offer.total_size = content.size();
    offer.sha256 = digest;
    offer.lazy_pull = is_lazy;

    Frame offer_frame{};
    offer_frame.header.type = FrameType::kClipOffer;
    offer_frame.header.stream_id = stream_id;
    offer_frame.header.flags = (is_lazy ? FrameFlags::kFin : FrameFlags::kNone) |
                               (is_pull_response ? FrameFlags::kPullResponse : FrameFlags::kNone);
    offer_frame.payload = offer.encode();

    if (!send_frame_fn_(offer_frame)) {
        if (is_lazy) {
            std::lock_guard<std::mutex> lock(mutex_);
            lazy_sources_.erase(clip_id);
        }
        return false;
    }

    bool ok = true;
    if (!is_lazy) {
        ok = stream_bytes_now(clip_id, stream_id, content);
    }

    if (ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_local_item_.clip_id = clip_id;
        latest_local_item_.mime_type = std::string(mime_type);
        latest_local_item_.inline_bytes.assign(content.begin(), content.end());
        latest_local_item_.file_path.clear();
        latest_local_item_.sha256 = digest;
        latest_local_item_.total_size = content.size();
        if (bypass_loopback || (last_received_hash_.has_value() && *last_received_hash_ != digest)) {
            last_received_hash_.reset();
        }
    }
    return ok;
}

bool ClipboardStreamManager::push_clipboard_file(
    std::string_view mime_type,
    const std::filesystem::path& file_path,
    bool lazy_pull,
    bool bypass_loopback,
    bool is_pull_response) {
    std::error_code ec;
    uint64_t fsize = std::filesystem::file_size(file_path, ec);
    if (ec) {
        return false;
    }
    auto digest_opt = sha256_file_range(file_path, 0, 0);
    if (!digest_opt) {
        return false;
    }
    if (!bypass_loopback && is_loopback_digest(*digest_opt)) {
        return false;
    }

    uint64_t clip_id = next_clip_id_.fetch_add(1);
    uint32_t stream_id = next_stream_id_.fetch_add(2);

    if (lazy_pull) {
        std::lock_guard<std::mutex> lock(mutex_);
        constexpr size_t kMaxLazySources = 64;
        while (lazy_sources_.size() >= kMaxLazySources) {
            auto oldest_it = lazy_sources_.begin();
            for (auto it = lazy_sources_.begin(); it != lazy_sources_.end(); ++it) {
                if (it->first < oldest_it->first) oldest_it = it;
            }
            lazy_sources_.erase(oldest_it);
        }
        LazySource src{};
        src.clip_id = clip_id;
        src.mime_type = std::string(mime_type);
        src.file_path = file_path;
        lazy_sources_[clip_id] = std::move(src);
    }

    ClipOfferPayload offer{};
    offer.clip_id = clip_id;
    offer.mime_type = std::string(mime_type);
    offer.total_size = fsize;
    offer.sha256 = *digest_opt;
    offer.lazy_pull = lazy_pull;

    Frame offer_frame{};
    offer_frame.header.type = FrameType::kClipOffer;
    offer_frame.header.stream_id = stream_id;
    offer_frame.header.flags = (lazy_pull ? FrameFlags::kFin : FrameFlags::kNone) |
                               (is_pull_response ? FrameFlags::kPullResponse : FrameFlags::kNone);
    offer_frame.payload = offer.encode();

    if (!send_frame_fn_(offer_frame)) {
        if (lazy_pull) {
            std::lock_guard<std::mutex> lock(mutex_);
            lazy_sources_.erase(clip_id);
        }
        return false;
    }

    bool ok = true;
    if (!lazy_pull) {
        ok = stream_file_now(clip_id, stream_id, file_path);
    }

    if (ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_local_item_.clip_id = clip_id;
        latest_local_item_.mime_type = std::string(mime_type);
        latest_local_item_.inline_bytes.clear();
        latest_local_item_.file_path = file_path;
        latest_local_item_.sha256 = *digest_opt;
        latest_local_item_.total_size = fsize;
        if (bypass_loopback || (last_received_hash_.has_value() && *last_received_hash_ != *digest_opt)) {
            last_received_hash_.reset();
        }
    }
    return ok;
}

bool ClipboardStreamManager::stream_bytes_now(
    uint64_t clip_id,
    uint32_t stream_id,
    std::span<const uint8_t> content) {
    size_t offset = 0;
    do {
        size_t chunk_len = std::min<size_t>(kMaxChunkSize, content.size() - offset);
        bool is_last = (offset + chunk_len >= content.size());

        ClipDataChunkPayload chunk{};
        chunk.clip_id = clip_id;
        chunk.offset = offset;
        chunk.data.assign(content.begin() + static_cast<std::ptrdiff_t>(offset),
                          content.begin() + static_cast<std::ptrdiff_t>(offset + chunk_len));

        Frame f{};
        f.header.type = FrameType::kClipDataChunk;
        f.header.stream_id = stream_id;
        f.header.flags = is_last ? FrameFlags::kFin : FrameFlags::kNone;
        f.payload = chunk.encode();

        if (!send_frame_fn_(f)) {
            return false;
        }
        offset += chunk_len;
    } while (offset < content.size());

    return true;
}

bool ClipboardStreamManager::stream_file_now(
    uint64_t clip_id,
    uint32_t stream_id,
    const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::array<uint8_t, kMaxChunkSize> buf{};
    uint64_t offset = 0;

    while (in.good()) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        if (in.bad()) {
            return false;
        }
        std::streamsize got = in.gcount();
        if (got <= 0 && offset > 0) {
            break;
        }
        bool is_last = (in.eof() || in.peek() == EOF);

        ClipDataChunkPayload chunk{};
        chunk.clip_id = clip_id;
        chunk.offset = offset;
        chunk.data.assign(buf.data(), buf.data() + got);

        Frame f{};
        f.header.type = FrameType::kClipDataChunk;
        f.header.stream_id = stream_id;
        f.header.flags = is_last ? FrameFlags::kFin : FrameFlags::kNone;
        f.payload = chunk.encode();

        if (!send_frame_fn_(f)) {
            return false;
        }
        offset += static_cast<uint64_t>(got);
        if (is_last) {
            break;
        }
    }
    return !in.bad();
}

bool ClipboardStreamManager::request_lazy_pull(uint64_t clip_id) {
    ClipPullReqPayload req{};
    req.clip_id = clip_id;

    Frame f{};
    f.header.type = FrameType::kClipPullReq;
    f.header.stream_id = next_stream_id_.fetch_add(2);
    f.header.flags = FrameFlags::kFin;
    f.payload = req.encode();
    return send_frame_fn_(f);
}

void ClipboardStreamManager::handle_frame(const Frame& frame) {
    switch (frame.header.type) {
        case FrameType::kClipOffer: {
            auto offer = ClipOfferPayload::decode(frame.payload);
            if (!offer) return;

            ClipboardLazyOfferCallback lazy_cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);

                IncomingStream stream{};
                stream.offer = *offer;
                stream.is_pull_response = (frame.header.flags & FrameFlags::kPullResponse) != 0;
                stream.use_temp_file = (offer->total_size > kMaxInlineMemoryBytes);
                if (stream.use_temp_file) {
                    std::error_code ec;
                    std::filesystem::create_directories(temp_dir_, ec);
                    stream.temp_file = temp_dir_ / ("l2a_clip_" + std::to_string(offer->clip_id) + ".tmp");
                    stream.temp_guard = std::make_shared<TempFileGuard>(stream.temp_file);
                    stream.temp_out = std::make_unique<std::ofstream>(
                        stream.temp_file, std::ios::binary | std::ios::trunc);
                    if (!stream.temp_out->is_open()) {
                        return;
                    }
                } else if (!offer->lazy_pull) {
                    stream.memory_buf.reserve(static_cast<size_t>(offer->total_size));
                }
                incoming_streams_[offer->clip_id] = std::move(stream);

                if (offer->lazy_pull) {
                    lazy_cb = on_lazy_offer_;
                }
            }
            if (lazy_cb) {
                lazy_cb(*offer);
            }
            break;
        }

        case FrameType::kClipDataChunk: {
            auto chunk = ClipDataChunkPayload::decode(frame.payload);
            if (!chunk) return;

            ClipboardReceivedCallback recv_cb;
            ReceivedClipboardItem completed_item{};
            bool finished = false;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = incoming_streams_.find(chunk->clip_id);
                if (it == incoming_streams_.end()) return;

                IncomingStream& st = it->second;
                if (chunk->offset != st.bytes_received) {
                    if (st.temp_out) {
                        st.temp_out->close();
                    }
                    incoming_streams_.erase(it);
                    return;
                }

                st.hasher.update(chunk->data);
                st.bytes_received += chunk->data.size();

                if (st.use_temp_file) {
                    if (!st.temp_out || !st.temp_out->is_open()) {
                        incoming_streams_.erase(it);
                        return;
                    }
                    if (!chunk->data.empty()) {
                        st.temp_out->write(
                            reinterpret_cast<const char*>(chunk->data.data()),
                            static_cast<std::streamsize>(chunk->data.size()));
                        if (!st.temp_out->good()) {
                            st.temp_out->close();
                            incoming_streams_.erase(it);
                            return;
                        }
                    }
                } else {
                    st.memory_buf.insert(st.memory_buf.end(),
                                         chunk->data.begin(),
                                         chunk->data.end());
                }

                bool is_fin = (frame.header.flags & FrameFlags::kFin) != 0 ||
                              (st.bytes_received >= st.offer.total_size);
                if (is_fin) {
                    if (st.temp_out) {
                        st.temp_out->flush();
                        bool write_ok = st.temp_out->good();
                        st.temp_out->close();
                        st.temp_out.reset();
                        if (!write_ok) {
                            incoming_streams_.erase(it);
                            return;
                        }
                    }
                    Sha256Digest calc_hash = st.hasher.finalize();
                    if (calc_hash == st.offer.sha256) {
                        last_received_hash_ = calc_hash;
                        completed_item.clip_id = st.offer.clip_id;
                        completed_item.mime_type = st.offer.mime_type;
                        completed_item.total_size = st.offer.total_size;
                        completed_item.sha256 = calc_hash;
                        completed_item.data = std::move(st.memory_buf);
                        completed_item.temp_file_path = st.temp_file;
                        completed_item.file_guard = std::move(st.temp_guard);
                        completed_item.is_pull_response = st.is_pull_response;
                        recv_cb = on_received_;
                        finished = true;
                    }
                    incoming_streams_.erase(it);
                }
            }

            if (finished && recv_cb) {
                recv_cb(completed_item);
            }
            break;
        }

        case FrameType::kClipPullReq: {
            auto req = ClipPullReqPayload::decode(frame.payload);
            if (!req) return;

            if (req->clip_id == 0) {
                // Request for latest clipboard: prefer live OS clipboard read first, fallback to cache.
                ClipboardPullRequestedCallback pull_cb;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pull_cb = on_pull_requested_;
                }
                if (pull_cb && pull_cb()) {
                    break;
                }

                LatestItem latest{};
                bool has_latest = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (latest_local_item_.clip_id != 0 &&
                        (!latest_local_item_.inline_bytes.empty() || !latest_local_item_.file_path.empty())) {
                        latest = latest_local_item_;
                        has_latest = true;
                    }
                }

                if (has_latest) {
                    uint64_t clip_id = next_clip_id_.fetch_add(1);
                    uint32_t stream_id = next_stream_id_.fetch_add(2);

                    ClipOfferPayload offer{};
                    offer.clip_id = clip_id;
                    offer.mime_type = latest.mime_type;
                    offer.total_size = latest.total_size;
                    offer.sha256 = latest.sha256;
                    offer.lazy_pull = false;

                    Frame offer_frame{};
                    offer_frame.header.type = FrameType::kClipOffer;
                    offer_frame.header.stream_id = stream_id;
                    offer_frame.header.flags = FrameFlags::kPullResponse;
                    offer_frame.payload = offer.encode();

                    if (send_frame_fn_(offer_frame)) {
                        if (!latest.file_path.empty()) {
                            stream_file_now(clip_id, stream_id, latest.file_path);
                        } else {
                            stream_bytes_now(clip_id, stream_id, latest.inline_bytes);
                        }
                    }
                }
                break;
            }

            LazySource src{};
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = lazy_sources_.find(req->clip_id);
                if (it != lazy_sources_.end()) {
                    src = it->second;
                    found = true;
                }
            }
            if (found) {
                uint32_t stream_id = next_stream_id_.fetch_add(2);
                if (!src.file_path.empty()) {
                    stream_file_now(src.clip_id, stream_id, src.file_path);
                } else {
                    stream_bytes_now(src.clip_id, stream_id, src.inline_bytes);
                }
            }
            break;
        }

        default:
            break;
    }
}

}  // namespace l2a
