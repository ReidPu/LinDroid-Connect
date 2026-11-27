#pragma once

#include "l2a/protocol.hpp"
#include "l2a/sha256.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace l2a {

using SendFrameFunc = std::function<bool(const Frame& frame)>;

struct TransferProgress {
    uint64_t batch_id{0};
    uint32_t file_index{0};
    std::string relative_path;
    uint64_t bytes_transferred{0};
    uint64_t total_file_bytes{0};
    bool is_sending{false};
};

using TransferProgressCallback = std::function<void(const TransferProgress& progress)>;
using FileCompletedCallback = std::function<void(
    uint64_t batch_id,
    uint32_t file_index,
    const std::filesystem::path& saved_path,
    bool is_sending,
    bool success,
    const std::string& error_msg)>;

class FileTransferManager {
public:
    FileTransferManager(
        SendFrameFunc send_frame_fn,
        std::filesystem::path receive_dir);
    ~FileTransferManager();

    void stop();

    void set_receive_dir(std::filesystem::path receive_dir);
    std::filesystem::path receive_dir() const;

    void set_callbacks(
        TransferProgressCallback on_progress,
        FileCompletedCallback on_file_completed);

    // Initiates transfer of files and/or directories (preserving folder hierarchy).
    // If custom_batch_id != 0, uses that batch_id (useful for resuming an interrupted batch).
    uint64_t send_paths(
        std::span<const std::filesystem::path> paths,
        uint64_t custom_batch_id = 0);

    // Processes incoming FILE_BATCH_META, FILE_RESUME_OFFSET, FILE_DATA_CHUNK, FILE_VERIFIED_ACK.
    void handle_frame(const Frame& frame);

    // Transport lifecycle hooks for automatic breakpoint resume & handle cleanup.
    void on_connection_lost();
    void on_connection_restored();

private:
    struct OutgoingFile {
        FileEntryMeta meta{};
        std::filesystem::path local_abs_path;
    };

    struct OutgoingBatch {
        uint64_t batch_id{0};
        uint32_t stream_id{0};
        uint64_t total_bytes{0};
        std::vector<OutgoingFile> files;
        std::unordered_set<uint32_t> acked_files;
        std::unordered_map<uint32_t, FileResumeEntry> accumulated_resume_entries;
        uint64_t active_epoch{0};
        bool interrupted{false};
        bool auto_resuming{false};
        int resume_attempts{0};
    };

    struct IncomingFileState {
        FileEntryMeta meta{};
        std::filesystem::path final_path;
        std::filesystem::path part_path;
        std::unique_ptr<std::ofstream> out_stream;
        uint64_t current_offset{0};
        bool completed{false};
        bool failed{false};
    };

    struct IncomingBatch {
        uint64_t batch_id{0};
        uint32_t total_files{0};
        uint64_t total_bytes{0};
        std::unordered_map<uint32_t, IncomingFileState> files;
        std::unordered_map<uint32_t, FileResumeEntry> resume_entries;
    };

    struct FinalizeTask {
        uint64_t batch_id{0};
        uint32_t file_index{0};
        std::filesystem::path part_path;
        std::filesystem::path final_path;
        Sha256Digest expected_hash{};
    };

    static std::filesystem::path sanitize_relative_path(std::string_view rel_path);
    static std::pair<std::filesystem::path, bool> resolve_destination_path(
        const std::filesystem::path& base_final,
        uint64_t expected_size,
        const Sha256Digest& expected_hash,
        const std::unordered_set<std::string>& reserved_paths);

    bool send_batch_meta_slices(uint64_t batch_id);
    void stream_outgoing_files(
        const OutgoingBatch& batch,
        const FileResumeOffsetPayload& resume_info);
    void finalize_incoming_file(const FinalizeTask& task);

    void enqueue_outgoing_stream(
        OutgoingBatch batch,
        FileResumeOffsetPayload resume_info);
    void enqueue_finalize(FinalizeTask task);
    void worker_loop();
    void finalize_loop();

    SendFrameFunc send_frame_fn_;
    std::filesystem::path receive_dir_;

    TransferProgressCallback on_progress_;
    FileCompletedCallback on_file_completed_;

    mutable std::mutex mutex_;
    std::atomic<uint64_t> next_batch_id_{1};
    std::atomic<uint32_t> next_stream_id_{1001};
    std::atomic<uint64_t> conn_epoch_{1};

    std::unordered_map<uint64_t, OutgoingBatch> outgoing_batches_;
    std::unordered_map<uint64_t, IncomingBatch> incoming_batches_;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<std::function<void()>> stream_queue_;
    std::thread worker_thread_;

    mutable std::mutex finalize_mutex_;
    std::condition_variable finalize_cv_;
    std::queue<FinalizeTask> finalize_queue_;
    std::thread finalize_thread_;

    std::atomic<bool> stopping_{false};
};

}  // namespace l2a
