#include "settings_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace l2a::linux_app {

namespace {

void skip_json_ws(std::string_view src, size_t& pos) {
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r')) {
        pos++;
    }
}

bool parse_json_bool(std::string_view src, size_t& pos, bool& out) {
    skip_json_ws(src, pos);
    if (src.substr(pos, 4) == "true") {
        out = true;
        pos += 4;
        return true;
    }
    if (src.substr(pos, 5) == "false") {
        out = false;
        pos += 5;
        return true;
    }
    return false;
}

bool parse_json_number(std::string_view src, size_t& pos, double& out) {
    skip_json_ws(src, pos);
    size_t start = pos;
    if (pos < src.size() && src[pos] == '-') pos++;
    while (pos < src.size() && ((src[pos] >= '0' && src[pos] <= '9') || src[pos] == '.')) {
        pos++;
    }
    if (start == pos) return false;
    std::string s(src.substr(start, pos - start));
    char* endp = nullptr;
    out = std::strtod(s.c_str(), &endp);
    return endp != s.c_str();
}

std::string parse_json_str(std::string_view src, size_t& pos) {
    skip_json_ws(src, pos);
    if (pos >= src.size() || src[pos] != '"') return "";
    pos++;
    std::string out;
    while (pos < src.size()) {
        char c = src[pos++];
        if (c == '"') return out;
        if (c == '\\' && pos < src.size()) {
            c = src[pos++];
        }
        out += c;
    }
    return out;
}

}  // namespace

SettingsManager::SettingsManager(std::filesystem::path config_dir) {
    if (config_dir.empty()) {
        const char* home = std::getenv("HOME");
        std::filesystem::path base = home ? std::filesystem::path(home) : std::filesystem::current_path();
        config_dir = base / ".config" / "linux_to_android";
    }
    file_path_ = config_dir / "settings.json";
    load();
}

AppSettings SettingsManager::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void SettingsManager::update(const AppSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
    ensure_defaults_locked();
    save_locked();
}

void SettingsManager::set_pc_to_phone_auto_share(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.pc_to_phone_auto_share = enabled;
    save_locked();
}

bool SettingsManager::pc_to_phone_auto_share() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_.pc_to_phone_auto_share;
}

void SettingsManager::set_phone_to_pc_auto_share(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.phone_to_pc_auto_share = enabled;
    save_locked();
}

bool SettingsManager::phone_to_pc_auto_share() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_.phone_to_pc_auto_share;
}

void SettingsManager::set_history_limit(size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.history_limit = std::clamp<size_t>(limit, 5, 200);
    save_locked();
}

size_t SettingsManager::history_limit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_.history_limit;
}

void SettingsManager::ensure_defaults_locked() {
    settings_.history_limit = std::clamp<size_t>(settings_.history_limit, 5, 200);
}

bool SettingsManager::save() {
    std::lock_guard<std::mutex> lock(mutex_);
    return save_locked();
}

bool SettingsManager::save_locked() {
    if (file_path_.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(file_path_.parent_path(), ec);

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"pc_to_phone_auto_share\": " << (settings_.pc_to_phone_auto_share ? "true" : "false") << ",\n";
    ss << "  \"phone_to_pc_auto_share\": " << (settings_.phone_to_pc_auto_share ? "true" : "false") << ",\n";
    ss << "  \"history_limit\": " << settings_.history_limit << "\n";
    ss << "}\n";

    std::filesystem::path tmp = file_path_.string() + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.is_open()) return false;
    out << ss.str();
    out.close();

    std::filesystem::rename(tmp, file_path_, ec);
    return !ec;
}

bool SettingsManager::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    if (!std::filesystem::exists(file_path_, ec)) {
        ensure_defaults_locked();
        // Create initial default settings file
        save_locked();
        return true;
    }

    std::ifstream in(file_path_);
    if (!in.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    size_t pos = 0;
    skip_json_ws(content, pos);
    if (pos >= content.size() || content[pos] != '{') {
        ensure_defaults_locked();
        save_locked();
        return false;
    }
    pos++;

    while (pos < content.size()) {
        skip_json_ws(content, pos);
        if (pos < content.size() && content[pos] == '}') {
            break;
        }
        std::string key = parse_json_str(content, pos);
        skip_json_ws(content, pos);
        if (pos < content.size() && content[pos] == ':') {
            pos++;
        }
        skip_json_ws(content, pos);

        if (key == "pc_to_phone_auto_share") {
            bool val = true;
            if (parse_json_bool(content, pos, val)) {
                settings_.pc_to_phone_auto_share = val;
            }
        } else if (key == "phone_to_pc_auto_share") {
            bool val = true;
            if (parse_json_bool(content, pos, val)) {
                settings_.phone_to_pc_auto_share = val;
            }
        } else if (key == "history_limit") {
            double num = 20;
            if (parse_json_number(content, pos, num)) {
                settings_.history_limit = static_cast<size_t>(num);
            }
        } else {
            // Skip unrecognized value
            while (pos < content.size() && content[pos] != ',' && content[pos] != '}') {
                pos++;
            }
        }

        skip_json_ws(content, pos);
        if (pos < content.size() && content[pos] == ',') {
            pos++;
        }
    }

    ensure_defaults_locked();
    return true;
}

}  // namespace l2a::linux_app
