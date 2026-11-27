#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>

namespace l2a::linux_app {

struct AppSettings {
    bool pc_to_phone_auto_share{true};
    bool phone_to_pc_auto_share{true};
    size_t history_limit{20};  // Clamped in [5, 200]
};

class SettingsManager {
public:
    explicit SettingsManager(std::filesystem::path config_dir = {});
    ~SettingsManager() = default;

    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    AppSettings get() const;
    void update(const AppSettings& settings);

    void set_pc_to_phone_auto_share(bool enabled);
    bool pc_to_phone_auto_share() const;

    void set_phone_to_pc_auto_share(bool enabled);
    bool phone_to_pc_auto_share() const;

    void set_history_limit(size_t limit);
    size_t history_limit() const;

    bool save();
    bool load();

    const std::filesystem::path& file_path() const noexcept { return file_path_; }

private:
    void ensure_defaults_locked();
    bool save_locked();

    std::filesystem::path file_path_;
    mutable std::mutex mutex_;
    AppSettings settings_{};
};

}  // namespace l2a::linux_app
