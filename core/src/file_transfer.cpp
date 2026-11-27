#include "l2a/file_transfer.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace l2a {

namespace {

constexpr int kMaxResumeAttempts = 3;

uint64_t initial_batch_id_seed() {
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    return ((now_ms & 0xFFFFFFFFULL) << 16U) | 1ULL;
}

}  // namespace

FileTransferManager::FileTransferManager(
    SendFrameFunc send_frame_fn,
    std::filesystem::path receive_dir)
    : send_frame_fn_(std::move(send_frame_fn)),
      receive_dir_(std::move(receive_dir)),
      next_batch_id_(initial_batch_id_seed()) {
    std::error_code ec;
    std::filesystem::create_directories(receive_dir_, ec);
    worker_thread_ = std::thread(&FileTransferManager::worker_loop, this);
    finalize_thread_ = std::thread(&FileTransferManager::finalize_loop, this);
}

FileTransferManager::~FileTransferManager() {
    stop();
}

void FileTransferManager::stop() {
    if (!stopping_.exchange(true)) {
        queue_cv_.notify_all();
        finalize_cv_.notify_all();
        if (worker_thread_.joinable()) {
            if (std::this_thread::get_id() != worker_thread_.get_id()) {
                worker_thread_.join();
            } else {
                worker_thread_.detach();
            }
        }
        if (finalize_thread_.joinable()) {
            if (std::this_thread::get_id() != finalize_thread_.get_id()) {
                finalize_thread_.join();
            } else {
                finalize_thread_.detach();
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [_, ib] : incoming_batches_) {
            for (auto& [__, st] : ib.files) {
                if (st.out_stream) {
                    st.out_stream->flush();
                    st.out_stream->close();
                    st.out_stream.reset();
                }
            }
        }
        incoming_batches_.clear();
    }
}

void FileTransferManager::worker_loop() {
    while (!stopping_.load()) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return stopping_.load() || !stream_queue_.empty();
            });
            if (stopping_.load()) break;
            task = std::move(stream_queue_.front());
            stream_queue_.pop();
        }
        if (task) {
            task();
        }
    }
}

void FileTransferManager::finalize_loop() {
    while (!stopping_.load()) {
        FinalizeTask task{};
        {
            std::unique_lock<std::mutex> lock(finalize_mutex_);
            finalize_cv_.wait(lock, [this] {
                return stopping_.load() || !finalize_queue_.empty();
            });
            if (stopping_.load()) break;
            task = std::move(finalize_queue_.front());
            finalize_queue_.pop();
        }
        finalize_incoming_file(task);
    }
}

void FileTransferManager::enqueue_outgoing_stream(
    OutgoingBatch batch,
    FileResumeOffsetPayload resume_info) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stopping_.load()) return;
        stream_queue_.push([this, b = std::move(batch), r = std::move(resume_info)]() {
            stream_outgoing_files(b, r);
        });
    }
    queue_cv_.notify_one();
}

void FileTransferManager::enqueue_finalize(FinalizeTask task) {
    {
        std::lock_guard<std::mutex> lock(finalize_mutex_);
        if (stopping_.load()) return;
        finalize_queue_.push(std::move(task));
    }
    finalize_cv_.notify_one();
}

void FileTransferManager::set_receive_dir(std::filesystem::path receive_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    receive_dir_ = std::move(receive_dir);
    std::error_code ec;
    std::filesystem::create_directories(receive_dir_, ec);
}

std::filesystem::path FileTransferManager::receive_dir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return receive_dir_;
}

void FileTransferManager::set_callbacks(
    TransferProgressCallback on_progress,
    FileCompletedCallback on_file_completed) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_progress_ = std::move(on_progress);
    on_file_completed_ = std::move(on_file_completed);
}

std::filesystem::path FileTransferManager::sanitize_relative_path(std::string_view rel_path) {
    std::filesystem::path raw(rel_path);
    std::filesystem::path clean;
    for (const auto& part : raw) {
        if (part == "." || part == ".." || part == "/") {
            continue;
        }
        clean /= part;
    }
    if (clean.empty()) {
        clean = "unnamed_file";
    }
    return clean;
}

std::pair<std::filesystem::path, bool> FileTransferManager::resolve_destination_path(
    const std::filesystem::path& base_final,
    uint64_t expected_size,
    const Sha256Digest& expected_hash,
    const std::unordered_set<std::string>& reserved_paths) {
    auto check_candidate = [&](const std::filesystem::path& candidate) -> std::optional<bool> {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            uint64_t fsz = std::filesystem::file_size(candidate, ec);
            if (!ec && fsz == expected_size) {
                auto existing_hash = sha256_file_range(candidate, 0, 0);
                if (existing_hash && *existing_hash == expected_hash) {
                    return true;  // Exact match already present
                }
            }
            return std::nullopt;  // Conflict with different content
        }
        if (reserved_paths.count(candidate.string()) > 0) {
            return std::nullopt;  // Reserved by another concurrent incoming file
        }
        return false;  // Available destination path
    };

    if (auto res = check_candidate(base_final)) {
        return {base_final, *res};
    }

    const std::filesystem::path parent = base_final.parent_path();
    const std::string stem = base_final.stem().string();
    const std::string ext = base_final.extension().string();

    for (int n = 1; n <= 10000; ++n) {
        std::filesystem::path candidate = parent / (stem + " (" + std::to_string(n) + ")" + ext);
        if (auto res = check_candidate(candidate)) {
            return {candidate, *res};
        }
    }
    return {base_final, false};
}

bool FileTransferManager::send_batch_meta_slices(uint64_t batch_id) {
    uint32_t sid = 0;
    uint32_t total_files_count = 0;
    uint64_t total_bytes_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = outgoing_batches_.find(batch_id);
        if (it == outgoing_batches_.end()) return false;
        sid = it->second.stream_id;
        total_files_count = static_cast<uint32_t>(it->second.files.size());
        total_bytes_count = it->second.total_bytes;
    }

    constexpr size_t kMaxPayloadBytesPerSlice = 48000;
    constexpr size_t kMaxFilesPerSlice = 128;
    constexpr size_t kBaseHeaderBytes = 24;
    constexpr size_t kEntryFixedBytes = 54;
    size_t offset = 0;

    do {
        std::vector<FileEntryMeta> slice_files;
        bool is_last = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = outgoing_batches_.find(batch_id);
            if (it == outgoing_batches_.end()) return false;
            const auto& all_files = it->second.files;

            size_t current_payload_bytes = kBaseHeaderBytes;
            size_t count = 0;
            while (offset + count < all_files.size()) {
                const auto& meta = all_files[offset + count].meta;
                size_t entry_bytes = kEntryFixedBytes + meta.relative_path.size();
                if (count > 0 &&
                    (current_payload_bytes + entry_bytes > kMaxPayloadBytesPerSlice ||
                     count >= kMaxFilesPerSlice)) {
                    break;
                }
                slice_files.push_back(meta);
                current_payload_bytes += entry_bytes;
                count++;
            }
            is_last = (offset + count >= all_files.size());
        }

        FileBatchMetaPayload meta_payload{};
        meta_payload.batch_id = batch_id;
        meta_payload.total_files = total_files_count;
        meta_payload.total_bytes = total_bytes_count;
        meta_payload.files = std::move(slice_files);

        Frame meta_frame{};
        meta_frame.header.type = FrameType::kFileBatchMeta;
        meta_frame.header.stream_id = sid;
        meta_frame.header.flags = is_last ? FrameFlags::kFin : FrameFlags::kNone;
        meta_frame.payload = meta_payload.encode();

        if (!send_frame_fn_(meta_frame)) {
            return false;
        }

        offset += meta_payload.files.size();
    } while (offset < total_files_count);

    return true;
}

uint64_t FileTransferManager::send_paths(
    std::span<const std::filesystem::path> paths,
    uint64_t custom_batch_id) {
    OutgoingBatch batch{};
    batch.batch_id = (custom_batch_id != 0) ? custom_batch_id : next_batch_id_.fetch_add(1);
    batch.stream_id = next_stream_id_.fetch_add(2);

    uint32_t idx = 0;
    auto add_single_file = [&](const std::filesystem::path& abs_path,
                               const std::filesystem::path& rel_path) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(abs_path, ec)) {
            return;
        }
        uint64_t fsize = std::filesystem::file_size(abs_path, ec);
        if (ec) return;

        auto digest_opt = sha256_file_range(abs_path, 0, 0);
        if (!digest_opt) return;

        OutgoingFile of{};
        of.local_abs_path = abs_path;
        of.meta.file_index = idx++;
        of.meta.relative_path = rel_path.generic_string();
        of.meta.file_size = fsize;
        of.meta.last_modified_ms = 0;
        of.meta.file_sha256 = *digest_opt;

        batch.total_bytes += fsize;
        batch.files.push_back(std::move(of));
    };

    for (const auto& p : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            std::filesystem::path base_parent = p.parent_path();
            for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(p, ec)) {
                if (dir_entry.is_regular_file(ec)) {
                    auto rel = std::filesystem::relative(dir_entry.path(), base_parent, ec);
                    if (!ec) {
                        add_single_file(dir_entry.path(), rel);
                    }
                }
            }
        } else if (std::filesystem::is_regular_file(p, ec)) {
            add_single_file(p, p.filename());
        }
    }

    if (batch.files.empty()) {
        return 0;
    }

    uint64_t bid = batch.batch_id;
    const uint64_t cur_epoch = conn_epoch_.load();
    batch.active_epoch = cur_epoch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // If caller explicitly re-sends the exact same file set after an interruption,
        // coalesce with an already-running auto-resume on the current live connection,
        // or supersede a stale interrupted batch.
        for (auto it = outgoing_batches_.begin(); it != outgoing_batches_.end();) {
            const auto& existing = it->second;
            if ((existing.interrupted || existing.auto_resuming) &&
                existing.files.size() == batch.files.size()) {
                bool same = true;
                for (size_t i = 0; i < existing.files.size(); ++i) {
                    if (existing.files[i].local_abs_path != batch.files[i].local_abs_path ||
                        existing.files[i].meta.file_sha256 != batch.files[i].meta.file_sha256) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    if (existing.auto_resuming && existing.active_epoch == cur_epoch) {
                        return existing.batch_id;
                    }
                    it = outgoing_batches_.erase(it);
                    continue;
                }
            }
            ++it;
        }
        outgoing_batches_[bid] = std::move(batch);
    }

    if (!send_batch_meta_slices(bid)) {
        std::lock_guard<std::mutex> lock(mutex_);
        outgoing_batches_.erase(bid);
        return 0;
    }

    return bid;
}

void FileTransferManager::on_connection_lost() {
    conn_epoch_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, ib] : incoming_batches_) {
        for (auto& [__, st] : ib.files) {
            if (st.out_stream) {
                st.out_stream->flush();
                st.out_stream->close();
                st.out_stream.reset();
            }
        }
    }
    incoming_batches_.clear();

    for (auto& [_, ob] : outgoing_batches_) {
        if (ob.acked_files.size() < ob.files.size()) {
            ob.interrupted = true;
            ob.auto_resuming = false;
            ob.accumulated_resume_entries.clear();
        }
    }
}

void FileTransferManager::on_connection_restored() {
    uint64_t new_epoch = conn_epoch_.fetch_add(1) + 1;
    std::vector<uint64_t> batches_to_resume;
    std::vector<std::pair<uint64_t, OutgoingBatch>> failed_batches;
    FileCompletedCallback comp_cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        comp_cb = on_file_completed_;
        for (auto it = outgoing_batches_.begin(); it != outgoing_batches_.end();) {
            auto& ob = it->second;
            if (ob.interrupted && ob.acked_files.size() < ob.files.size()) {
                if (ob.resume_attempts >= kMaxResumeAttempts) {
                    failed_batches.emplace_back(it->first, std::move(ob));
                    it = outgoing_batches_.erase(it);
                    continue;
                }
                ob.resume_attempts++;
                ob.interrupted = false;
                ob.auto_resuming = true;
                ob.active_epoch = new_epoch;
                ob.accumulated_resume_entries.clear();
                batches_to_resume.push_back(it->first);
            }
            ++it;
        }
    }

    if (comp_cb) {
        for (const auto& [bid, ob] : failed_batches) {
            for (const auto& f : ob.files) {
                if (ob.acked_files.count(f.meta.file_index) == 0) {
                    comp_cb(bid, f.meta.file_index, f.local_abs_path, true, false,
                            "Connection interrupted (max resume attempts exceeded)");
                }
            }
        }
    }

    if (batches_to_resume.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stopping_.load()) return;
        for (uint64_t bid : batches_to_resume) {
            stream_queue_.push([this, bid]() {
                if (stopping_.load()) return;
                if (!send_batch_meta_slices(bid)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = outgoing_batches_.find(bid);
                    if (it != outgoing_batches_.end()) {
                        it->second.interrupted = true;
                        it->second.auto_resuming = false;
                    }
                }
            });
        }
    }
    queue_cv_.notify_all();
}

void FileTransferManager::handle_frame(const Frame& frame) {
    switch (frame.header.type) {
        case FrameType::kFileBatchMeta: {
            auto meta = FileBatchMetaPayload::decode(frame.payload);
            if (!meta) return;

            std::vector<FinalizeTask> immediate_finalize_files;
            bool all_meta_received = false;
            FileResumeOffsetPayload full_resume{};
            full_resume.batch_id = meta->batch_id;
            uint32_t stream_id = frame.header.stream_id;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = incoming_batches_.find(meta->batch_id);
                if (it == incoming_batches_.end()) {
                    IncomingBatch ib{};
                    ib.batch_id = meta->batch_id;
                    ib.total_files = meta->total_files;
                    ib.total_bytes = meta->total_bytes;
                    incoming_batches_[meta->batch_id] = std::move(ib);
                    it = incoming_batches_.find(meta->batch_id);
                }

                IncomingBatch& ib = it->second;
                if (ib.total_files == 0 && meta->total_files > 0) {
                    ib.total_files = meta->total_files;
                    ib.total_bytes = meta->total_bytes;
                }

                // Collect reserved final paths and active part paths from other concurrent incoming files
                std::unordered_set<std::string> reserved_final_paths;
                std::unordered_set<std::string> active_part_paths;
                for (const auto& [other_bid, other_ib] : incoming_batches_) {
                    for (const auto& [other_fidx, other_st] : other_ib.files) {
                        if (other_bid == meta->batch_id && other_fidx == 0) {
                            // still insert if not same entry
                        }
                        if (!other_st.completed && !other_st.failed) {
                            reserved_final_paths.insert(other_st.final_path.string());
                            active_part_paths.insert(other_st.part_path.string());
                        }
                    }
                }

                for (const auto& fmeta : meta->files) {
                    // If duplicate meta slice for an already-tracked file in this batch, skip recreating
                    if (ib.files.find(fmeta.file_index) != ib.files.end()) {
                        continue;
                    }

                    IncomingFileState st{};
                    st.meta = fmeta;
                    auto clean_rel = sanitize_relative_path(fmeta.relative_path);
                    std::filesystem::path base_final = receive_dir_ / clean_rel;

                    if (base_final.has_parent_path()) {
                        std::error_code ec;
                        std::filesystem::create_directories(base_final.parent_path(), ec);
                    }

                    auto [resolved_final, already_complete] = resolve_destination_path(
                        base_final, fmeta.file_size, fmeta.file_sha256, reserved_final_paths);

                    st.final_path = resolved_final;
                    reserved_final_paths.insert(st.final_path.string());

                    st.part_path = st.final_path;
                    st.part_path += ".l2apart";
                    if (!already_complete && active_part_paths.count(st.part_path.string()) > 0) {
                        st.part_path += ".b" + std::to_string(meta->batch_id);
                    }
                    active_part_paths.insert(st.part_path.string());

                    FileResumeEntry r_entry{};
                    r_entry.file_index = fmeta.file_index;
                    r_entry.resume_offset = 0;

                    std::error_code ec;
                    if (already_complete) {
                        st.current_offset = fmeta.file_size;
                        st.completed = true;
                        r_entry.resume_offset = fmeta.file_size;
                        immediate_finalize_files.push_back({
                            meta->batch_id,
                            fmeta.file_index,
                            st.part_path,
                            st.final_path,
                            fmeta.file_sha256
                        });
                    }

                    // Check partial .l2apart file for resume (including part_size == file_size!)
                    if (!st.completed && std::filesystem::exists(st.part_path, ec)) {
                        uint64_t part_size = std::filesystem::file_size(st.part_path, ec);
                        if (!ec && part_size > 0 && part_size < fmeta.file_size) {
                            uint64_t tail_len = std::min<uint64_t>(kMaxChunkSize, part_size);
                            uint64_t tail_off = part_size - tail_len;
                            auto tail_hash = sha256_file_range(st.part_path, tail_off, tail_len);
                            if (tail_hash) {
                                st.current_offset = part_size;
                                r_entry.resume_offset = part_size;
                                r_entry.tail_block_sha256 = *tail_hash;
                            }
                        } else if (!ec && part_size == fmeta.file_size && fmeta.file_size > 0) {
                            auto full_part_hash = sha256_file_range(st.part_path, 0, 0);
                            if (full_part_hash && *full_part_hash == fmeta.file_sha256) {
                                st.current_offset = fmeta.file_size;
                                st.completed = true;
                                r_entry.resume_offset = fmeta.file_size;
                                immediate_finalize_files.push_back({
                                    meta->batch_id,
                                    fmeta.file_index,
                                    st.part_path,
                                    st.final_path,
                                    fmeta.file_sha256
                                });
                            } else {
                                // Corrupted full-size .l2apart -> purge and restart from offset 0
                                std::filesystem::remove(st.part_path, ec);
                            }
                        } else if (!ec && part_size > fmeta.file_size) {
                            // Oversized stale .l2apart -> purge and restart from offset 0
                            std::filesystem::remove(st.part_path, ec);
                        }
                    }

                    if (!st.completed && r_entry.resume_offset == 0) {
                        std::ofstream trunc_part(st.part_path, std::ios::binary | std::ios::trunc);
                        st.current_offset = 0;
                    }

                    if (fmeta.file_size == 0 && !st.completed) {
                        st.completed = true;
                        immediate_finalize_files.push_back({
                            meta->batch_id,
                            fmeta.file_index,
                            st.part_path,
                            st.final_path,
                            fmeta.file_sha256
                        });
                    }

                    ib.resume_entries[fmeta.file_index] = r_entry;
                    ib.files[fmeta.file_index] = std::move(st);
                }

                bool is_fin = (frame.header.flags & FrameFlags::kFin) != 0;
                if ((ib.total_files > 0 && ib.files.size() >= ib.total_files) || is_fin) {
                    all_meta_received = true;
                    full_resume.entries.reserve(ib.resume_entries.size());
                    for (const auto& [_, re] : ib.resume_entries) {
                        full_resume.entries.push_back(re);
                    }
                }
            }

            if (all_meta_received) {
                constexpr size_t kMaxResumePerFrame = 400;
                size_t total_entries = full_resume.entries.size();
                size_t offset = 0;
                do {
                    size_t count = std::min(kMaxResumePerFrame, total_entries - offset);
                    bool is_last = (offset + count >= total_entries);

                    FileResumeOffsetPayload slice{};
                    slice.batch_id = full_resume.batch_id;
                    slice.entries.assign(full_resume.entries.begin() + offset,
                                         full_resume.entries.begin() + offset + count);

                    Frame resp_frame{};
                    resp_frame.header.type = FrameType::kFileResumeOffset;
                    resp_frame.header.stream_id = stream_id;
                    resp_frame.header.flags = is_last ? FrameFlags::kFin : FrameFlags::kNone;
                    resp_frame.payload = slice.encode();
                    send_frame_fn_(resp_frame);

                    offset += count;
                } while (offset < total_entries);
            }

            for (auto& ff : immediate_finalize_files) {
                enqueue_finalize(std::move(ff));
            }
            break;
        }

        case FrameType::kFileResumeOffset: {
            auto resume = FileResumeOffsetPayload::decode(frame.payload);
            if (!resume) return;

            OutgoingBatch batch_copy{};
            FileResumeOffsetPayload full_resume{};
            bool ready_to_stream = false;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = outgoing_batches_.find(resume->batch_id);
                if (it != outgoing_batches_.end()) {
                    for (const auto& rentry : resume->entries) {
                        it->second.accumulated_resume_entries[rentry.file_index] = rentry;
                    }
                    bool is_fin = (frame.header.flags & FrameFlags::kFin) != 0;
                    if ((it->second.files.size() > 0 &&
                         it->second.accumulated_resume_entries.size() >= it->second.files.size()) ||
                        is_fin) {
                        it->second.auto_resuming = false;
                        batch_copy = it->second;
                        full_resume.batch_id = it->second.batch_id;
                        full_resume.entries.reserve(it->second.accumulated_resume_entries.size());
                        for (const auto& [_, re] : it->second.accumulated_resume_entries) {
                            full_resume.entries.push_back(re);
                        }
                        ready_to_stream = true;
                    }
                }
            }
            if (ready_to_stream) {
                enqueue_outgoing_stream(std::move(batch_copy), std::move(full_resume));
            }
            break;
        }

        case FrameType::kFileDataChunk: {
            auto chunk = FileDataChunkPayload::decode(frame.payload);
            if (!chunk) return;

            TransferProgressCallback prog_cb;
            TransferProgress prog{};
            bool do_progress = false;

            bool should_finalize = false;
            FinalizeTask fin_task{};

            bool disk_error = false;
            uint64_t err_bid = chunk->batch_id;
            uint32_t err_fidx = chunk->file_index;
            std::filesystem::path err_final_path;
            FileCompletedCallback err_comp_cb;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto bit = incoming_batches_.find(chunk->batch_id);
                if (bit == incoming_batches_.end()) return;
                auto fit = bit->second.files.find(chunk->file_index);
                if (fit == bit->second.files.end()) return;

                IncomingFileState& st = fit->second;
                if (st.completed || st.failed) return;

                if (chunk->offset == 0 && st.current_offset != 0) {
                    if (st.out_stream) {
                        st.out_stream->close();
                        st.out_stream.reset();
                    }
                    std::ofstream trunc_part(st.part_path, std::ios::binary | std::ios::trunc);
                    st.current_offset = 0;
                }

                if (chunk->offset != st.current_offset) {
                    return;
                }

                if (!chunk->data.empty()) {
                    if (!st.out_stream || !st.out_stream->is_open()) {
                        std::ios::openmode mode = std::ios::binary |
                            (st.current_offset == 0 ? std::ios::trunc : std::ios::app);
                        st.out_stream = std::make_unique<std::ofstream>(st.part_path, mode);
                    }
                    if (!st.out_stream->is_open()) {
                        disk_error = true;
                    } else {
                        st.out_stream->write(
                            reinterpret_cast<const char*>(chunk->data.data()),
                            static_cast<std::streamsize>(chunk->data.size()));
                        if (!st.out_stream->good()) {
                            disk_error = true;
                        } else {
                            st.current_offset += chunk->data.size();
                        }
                    }

                    if (disk_error) {
                        if (st.out_stream) {
                            st.out_stream->close();
                            st.out_stream.reset();
                        }
                        std::error_code ec;
                        std::filesystem::remove(st.part_path, ec);
                        st.failed = true;
                        st.current_offset = 0;
                        err_final_path = st.final_path;
                        err_comp_cb = on_file_completed_;
                    }
                }

                if (!disk_error) {
                    if (on_progress_) {
                        prog_cb = on_progress_;
                        prog.batch_id = chunk->batch_id;
                        prog.file_index = chunk->file_index;
                        prog.relative_path = st.meta.relative_path;
                        prog.bytes_transferred = st.current_offset;
                        prog.total_file_bytes = st.meta.file_size;
                        prog.is_sending = false;
                        do_progress = true;
                    }

                    if (st.current_offset >= st.meta.file_size) {
                        if (st.out_stream) {
                            st.out_stream->flush();
                            bool flush_ok = st.out_stream->good();
                            st.out_stream->close();
                            st.out_stream.reset();
                            if (!flush_ok) {
                                disk_error = true;
                                std::error_code ec;
                                std::filesystem::remove(st.part_path, ec);
                                st.failed = true;
                                st.current_offset = 0;
                                err_final_path = st.final_path;
                                err_comp_cb = on_file_completed_;
                            }
                        }
                        if (!disk_error) {
                            should_finalize = true;
                            fin_task.batch_id = chunk->batch_id;
                            fin_task.file_index = st.meta.file_index;
                            fin_task.part_path = st.part_path;
                            fin_task.final_path = st.final_path;
                            fin_task.expected_hash = st.meta.file_sha256;
                            st.completed = true;
                        }
                    }
                }
            }

            if (disk_error) {
                FileVerifiedAckPayload ack{};
                ack.batch_id = err_bid;
                ack.file_index = err_fidx;
                ack.success = false;
                ack.error_message = "Disk write error";

                Frame ack_frame{};
                ack_frame.header.type = FrameType::kFileVerifiedAck;
                ack_frame.header.stream_id = 0;
                ack_frame.header.flags = FrameFlags::kFin;
                ack_frame.payload = ack.encode();
                send_frame_fn_(ack_frame);

                if (err_comp_cb) {
                    err_comp_cb(err_bid, err_fidx, err_final_path, false, false, "Disk write error");
                }
                return;
            }

            if (do_progress && prog_cb) {
                prog_cb(prog);
            }

            if (should_finalize) {
                enqueue_finalize(std::move(fin_task));
            }
            break;
        }

        case FrameType::kFileVerifiedAck: {
            auto ack = FileVerifiedAckPayload::decode(frame.payload);
            if (!ack) return;

            FileCompletedCallback comp_cb;
            std::filesystem::path local_path;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                comp_cb = on_file_completed_;
                auto bit = outgoing_batches_.find(ack->batch_id);
                if (bit != outgoing_batches_.end()) {
                    if (ack->file_index < bit->second.files.size()) {
                        local_path = bit->second.files[ack->file_index].local_abs_path;
                    }
                    bit->second.acked_files.insert(ack->file_index);
                    if (bit->second.acked_files.size() >= bit->second.files.size()) {
                        outgoing_batches_.erase(bit);
                    }
                }
            }
            if (comp_cb) {
                comp_cb(ack->batch_id, ack->file_index, local_path, true, ack->success, ack->error_message);
            }
            break;
        }

        default:
            break;
    }
}

void FileTransferManager::stream_outgoing_files(
    const OutgoingBatch& batch,
    const FileResumeOffsetPayload& resume_info) {
    std::unordered_map<uint32_t, FileResumeEntry> resume_map;
    for (const auto& e : resume_info.entries) {
        resume_map[e.file_index] = e;
    }

    std::array<uint8_t, kMaxChunkSize> buf{};

    for (const auto& ofile : batch.files) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto bit = outgoing_batches_.find(batch.batch_id);
            if (bit == outgoing_batches_.end()) {
                return;  // Batch completed or superseded
            }
            if (bit->second.acked_files.count(ofile.meta.file_index) > 0) {
                continue;
            }
        }

        uint64_t start_offset = 0;
        auto rit = resume_map.find(ofile.meta.file_index);
        if (rit != resume_map.end()) {
            const auto& rentry = rit->second;
            if (rentry.resume_offset >= ofile.meta.file_size && ofile.meta.file_size > 0) {
                continue;  // Receiver already has verified full file
            }
            if (rentry.resume_offset > 0 && rentry.resume_offset < ofile.meta.file_size) {
                uint64_t tail_len = std::min<uint64_t>(kMaxChunkSize, rentry.resume_offset);
                uint64_t tail_off = rentry.resume_offset - tail_len;
                auto local_tail = sha256_file_range(ofile.local_abs_path, tail_off, tail_len);
                if (local_tail && *local_tail == rentry.tail_block_sha256) {
                    start_offset = rentry.resume_offset;
                }
            }
        }

        std::ifstream in(ofile.local_abs_path, std::ios::binary);
        if (!in.is_open()) {
            continue;
        }
        in.seekg(static_cast<std::streamoff>(start_offset), std::ios::beg);
        uint64_t offset = start_offset;

        while (in.good() && offset < ofile.meta.file_size) {
            if (stopping_.load() || conn_epoch_.load() != batch.active_epoch) return;
            in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
            std::streamsize got = in.gcount();
            if (got <= 0) break;

            bool is_last = (offset + static_cast<uint64_t>(got) >= ofile.meta.file_size);

            FileDataChunkPayload chunk{};
            chunk.batch_id = batch.batch_id;
            chunk.file_index = ofile.meta.file_index;
            chunk.offset = offset;
            chunk.data.assign(buf.data(), buf.data() + got);

            Frame f{};
            f.header.type = FrameType::kFileDataChunk;
            f.header.stream_id = batch.stream_id;
            f.header.flags = is_last ? FrameFlags::kFin : FrameFlags::kNone;
            f.payload = chunk.encode();

            if (!send_frame_fn_(f)) {
                std::vector<FileEntryMeta> unacked_to_fail;
                std::vector<std::filesystem::path> unacked_paths;
                FileCompletedCallback comp_cb;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto bit = outgoing_batches_.find(batch.batch_id);
                    if (bit != outgoing_batches_.end() &&
                        bit->second.active_epoch == batch.active_epoch) {
                        if (bit->second.resume_attempts >= kMaxResumeAttempts) {
                            comp_cb = on_file_completed_;
                            for (const auto& fitem : bit->second.files) {
                                if (bit->second.acked_files.count(fitem.meta.file_index) == 0) {
                                    unacked_to_fail.push_back(fitem.meta);
                                    unacked_paths.push_back(fitem.local_abs_path);
                                }
                            }
                            outgoing_batches_.erase(bit);
                        } else {
                            bit->second.interrupted = true;
                            bit->second.accumulated_resume_entries.clear();
                        }
                    }
                }
                if (comp_cb) {
                    for (size_t i = 0; i < unacked_to_fail.size(); ++i) {
                        comp_cb(batch.batch_id, unacked_to_fail[i].file_index, unacked_paths[i],
                                true, false, "Connection interrupted");
                    }
                }
                return;
            }

            offset += static_cast<uint64_t>(got);

            TransferProgressCallback prog_cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                prog_cb = on_progress_;
            }
            if (prog_cb) {
                TransferProgress prog{};
                prog.batch_id = batch.batch_id;
                prog.file_index = ofile.meta.file_index;
                prog.relative_path = ofile.meta.relative_path;
                prog.bytes_transferred = offset;
                prog.total_file_bytes = ofile.meta.file_size;
                prog.is_sending = true;
                prog_cb(prog);
            }
        }
    }
}

void FileTransferManager::finalize_incoming_file(const FinalizeTask& task) {
    bool ok = false;
    std::string err_msg;
    std::error_code ec;
    std::filesystem::path actual_final_path = task.final_path;

    if (std::filesystem::exists(actual_final_path, ec)) {
        auto final_hash = sha256_file_range(actual_final_path, 0, 0);
        if (final_hash.has_value() && (*final_hash == task.expected_hash)) {
            ok = true;
            if (std::filesystem::exists(task.part_path, ec)) {
                std::filesystem::remove(task.part_path, ec);
            }
        }
    }

    if (!ok) {
        auto calc_hash = sha256_file_range(task.part_path, 0, 0);
        ok = calc_hash.has_value() && (*calc_hash == task.expected_hash);
        if (ok) {
            if (std::filesystem::exists(actual_final_path, ec)) {
                std::unordered_set<std::string> empty_reserved;
                auto [new_path, already_matched] = resolve_destination_path(
                    actual_final_path,
                    std::filesystem::file_size(task.part_path, ec),
                    task.expected_hash,
                    empty_reserved);
                actual_final_path = new_path;
                if (already_matched) {
                    std::filesystem::remove(task.part_path, ec);
                } else {
                    std::filesystem::rename(task.part_path, actual_final_path, ec);
                }
            } else {
                std::filesystem::rename(task.part_path, actual_final_path, ec);
            }
            if (ec) {
                ok = false;
                err_msg = ec.message();
            }
        } else {
            err_msg = "SHA-256 verification failed";
            std::filesystem::remove(task.part_path, ec);
        }
    }

    FileVerifiedAckPayload ack{};
    ack.batch_id = task.batch_id;
    ack.file_index = task.file_index;
    ack.success = ok;
    ack.error_message = err_msg;

    Frame ack_frame{};
    ack_frame.header.type = FrameType::kFileVerifiedAck;
    ack_frame.header.stream_id = 0;
    ack_frame.header.flags = FrameFlags::kFin;
    ack_frame.payload = ack.encode();
    send_frame_fn_(ack_frame);

    FileCompletedCallback comp_cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        comp_cb = on_file_completed_;
        auto bit = incoming_batches_.find(task.batch_id);
        if (bit != incoming_batches_.end()) {
            auto fit = bit->second.files.find(task.file_index);
            if (fit != bit->second.files.end()) {
                if (!ok) {
                    fit->second.completed = false;
                    fit->second.failed = true;
                    fit->second.current_offset = 0;
                } else {
                    fit->second.completed = true;
                    fit->second.final_path = actual_final_path;
                }
            }
            if (ok) {
                bool all_done = true;
                for (const auto& [idx, f] : bit->second.files) {
                    if (!f.completed) {
                        all_done = false;
                        break;
                    }
                }
                if (all_done && bit->second.files.size() >= bit->second.total_files && bit->second.total_files > 0) {
                    incoming_batches_.erase(bit);
                }
            }
        }
    }

    if (comp_cb) {
        comp_cb(task.batch_id, task.file_index, actual_final_path, false, ok, err_msg);
    }
}

}  // namespace l2a
