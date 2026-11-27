#include "l2a/history_manager.hpp"
#include "l2a/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace l2a {

namespace {

std::string escape_json_string(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string hex_encode(std::span<const uint8_t> data) {
    std::string out;
    out.reserve(data.size() * 2);
    static const char kHexChars[] = "0123456789abcdef";
    for (uint8_t b : data) {
        out.push_back(kHexChars[(b >> 4) & 0x0F]);
        out.push_back(kHexChars[b & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> hex_decode(std::string_view hex) {
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto from_hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = from_hex(hex[i]);
        int l = from_hex(hex[i + 1]);
        if (h < 0 || l < 0) return {};
        out.push_back(static_cast<uint8_t>((h << 4) | l));
    }
    return out;
}

// Simple JSON Tokenizer & Parser
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type{Type::Null};
    bool bool_val{false};
    double num_val{0.0};
    std::string str_val;
    std::vector<JsonValue> array_val;
    std::vector<std::pair<std::string, JsonValue>> obj_val;

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : obj_val) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

class SimpleJsonParser {
public:
    explicit SimpleJsonParser(std::string_view src) : src_(src), pos_(0) {}

    bool parse(JsonValue& root) {
        skip_ws();
        return parse_value(root);
    }

private:
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos_++;
            } else {
                break;
            }
        }
    }

    bool parse_value(JsonValue& val) {
        skip_ws();
        if (pos_ >= src_.size()) return false;
        char c = src_[pos_];
        if (c == '{') return parse_object(val);
        if (c == '[') return parse_array(val);
        if (c == '"') return parse_string(val);
        if (c == 't' || c == 'f') return parse_bool(val);
        if (c == 'n') return parse_null(val);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(val);
        return false;
    }

    bool parse_object(JsonValue& val) {
        val.type = JsonValue::Type::Object;
        val.obj_val.clear();
        pos_++;  // skip '{'
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            pos_++;
            return true;
        }
        while (pos_ < src_.size()) {
            JsonValue key_val;
            if (!parse_string(key_val)) return false;
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != ':') return false;
            pos_++;  // skip ':'
            JsonValue child;
            if (!parse_value(child)) return false;
            val.obj_val.emplace_back(std::move(key_val.str_val), std::move(child));
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                pos_++;
                return true;
            }
            if (pos_ < src_.size() && src_[pos_] == ',') {
                pos_++;
                skip_ws();
            } else {
                return false;
            }
        }
        return false;
    }

    bool parse_array(JsonValue& val) {
        val.type = JsonValue::Type::Array;
        val.array_val.clear();
        pos_++;  // skip '['
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            pos_++;
            return true;
        }
        while (pos_ < src_.size()) {
            JsonValue item;
            if (!parse_value(item)) return false;
            val.array_val.push_back(std::move(item));
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                pos_++;
                return true;
            }
            if (pos_ < src_.size() && src_[pos_] == ',') {
                pos_++;
                skip_ws();
            } else {
                return false;
            }
        }
        return false;
    }

    bool parse_string(JsonValue& val) {
        val.type = JsonValue::Type::String;
        val.str_val.clear();
        if (pos_ >= src_.size() || src_[pos_] != '"') return false;
        pos_++;  // skip '"'
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= src_.size()) return false;
                char esc = src_[pos_++];
                switch (esc) {
                    case '"': val.str_val += '"'; break;
                    case '\\': val.str_val += '\\'; break;
                    case '/': val.str_val += '/'; break;
                    case 'b': val.str_val += '\b'; break;
                    case 'f': val.str_val += '\f'; break;
                    case 'n': val.str_val += '\n'; break;
                    case 'r': val.str_val += '\r'; break;
                    case 't': val.str_val += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) return false;
                        std::string hex_str(src_.substr(pos_, 4));
                        pos_ += 4;
                        unsigned int ucode = 0;
                        if (std::sscanf(hex_str.c_str(), "%x", &ucode) == 1) {
                            if (ucode <= 0x7F) {
                                val.str_val += static_cast<char>(ucode);
                            } else {
                                // Basic multi-byte utf8 encoding
                                if (ucode <= 0x7FF) {
                                    val.str_val += static_cast<char>(0xC0 | ((ucode >> 6) & 0x1F));
                                    val.str_val += static_cast<char>(0x80 | (ucode & 0x3F));
                                } else {
                                    val.str_val += static_cast<char>(0xE0 | ((ucode >> 12) & 0x0F));
                                    val.str_val += static_cast<char>(0x80 | ((ucode >> 6) & 0x3F));
                                    val.str_val += static_cast<char>(0x80 | (ucode & 0x3F));
                                }
                            }
                        }
                        break;
                    }
                    default:
                        val.str_val += esc;
                        break;
                }
            } else {
                val.str_val += c;
            }
        }
        return false;
    }

    bool parse_bool(JsonValue& val) {
        val.type = JsonValue::Type::Bool;
        if (src_.substr(pos_, 4) == "true") {
            val.bool_val = true;
            pos_ += 4;
            return true;
        }
        if (src_.substr(pos_, 5) == "false") {
            val.bool_val = false;
            pos_ += 5;
            return true;
        }
        return false;
    }

    bool parse_null(JsonValue& val) {
        val.type = JsonValue::Type::Null;
        if (src_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return true;
        }
        return false;
    }

    bool parse_number(JsonValue& val) {
        val.type = JsonValue::Type::Number;
        size_t start = pos_;
        if (src_[pos_] == '-') pos_++;
        while (pos_ < src_.size() && ((src_[pos_] >= '0' && src_[pos_] <= '9') || src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E' || src_[pos_] == '+' || src_[pos_] == '-')) {
            pos_++;
        }
        std::string num_str(src_.substr(start, pos_ - start));
        char* endp = nullptr;
        val.num_val = std::strtod(num_str.c_str(), &endp);
        return endp != num_str.c_str();
    }

    std::string_view src_;
    size_t pos_{0};
};

}  // namespace

ClipboardHistoryManager::ClipboardHistoryManager(
    std::filesystem::path history_file_path,
    size_t max_limit)
    : history_file_path_(std::move(history_file_path)),
      max_limit_(std::clamp<size_t>(max_limit, 5, 200)) {
    if (!history_file_path_.empty()) {
        load_from_disk();
    }
}

uint64_t ClipboardHistoryManager::add_text_item(
    ClipOrigin origin,
    std::string_view text,
    const Sha256Digest& sha256) {
    ClipboardHistoryItem item{};
    item.type = ClipContentType::Text;
    item.origin = origin;
    item.mime_type = "text/plain";
    item.full_text = std::string(text);
    item.text_preview = text.substr(0, std::min<size_t>(80, text.size()));
    item.sha256 = sha256;
    item.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return add_item(std::move(item));
}

std::string extension_for_mime(std::string_view mime_type) {
    if (mime_type.find("jpeg") != std::string_view::npos || mime_type.find("jpg") != std::string_view::npos) {
        return ".jpg";
    }
    if (mime_type.find("webp") != std::string_view::npos) {
        return ".webp";
    }
    if (mime_type.find("gif") != std::string_view::npos) {
        return ".gif";
    }
    if (mime_type.find("bmp") != std::string_view::npos) {
        return ".bmp";
    }
    return ".png";
}

std::filesystem::path resolve_clip_cache_dir(const std::filesystem::path& history_file_path) {
    std::filesystem::path cache_dir;
    if (!history_file_path.empty()) {
        cache_dir = history_file_path.parent_path() / "clip_cache";
    } else {
        const char* home = std::getenv("HOME");
        cache_dir = home ? (std::filesystem::path(home) / ".config" / "linux_to_android" / "clip_cache")
                         : (std::filesystem::current_path() / ".config" / "linux_to_android" / "clip_cache");
    }
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        cache_dir = std::filesystem::temp_directory_path() / "l2a_clip_cache";
        ec.clear();
        std::filesystem::create_directories(cache_dir, ec);
    }
    return cache_dir;
}

uint64_t ClipboardHistoryManager::add_image_item(
    ClipOrigin origin,
    std::string_view mime_type,
    std::span<const uint8_t> data,
    const std::filesystem::path& file_path,
    const Sha256Digest& sha256) {
    ClipboardHistoryItem item{};
    item.type = ClipContentType::Image;
    item.origin = origin;
    item.mime_type = std::string(mime_type);
    item.image_path = file_path;
    item.sha256 = sha256;

    const std::string ext = extension_for_mime(mime_type);
    const std::filesystem::path cache_dir = resolve_clip_cache_dir(history_file_path_);
    const std::filesystem::path cached_path = cache_dir / ("img_" + to_hex(sha256) + ext);

    std::error_code ec;
    if (!item.image_path.empty()) {
        const std::string fname = item.image_path.filename().string();
        const bool is_transient_temp =
            (item.image_path.extension() == ".tmp") ||
            (fname.rfind("l2a_clip_", 0) == 0) ||
            (item.image_path.parent_path().filename() == "tmp_clip");
        if (is_transient_temp && std::filesystem::exists(item.image_path, ec)) {
            if (!std::filesystem::exists(cached_path, ec)) {
                std::filesystem::copy_file(
                    item.image_path, cached_path, std::filesystem::copy_options::skip_existing, ec);
            }
            if (std::filesystem::exists(cached_path, ec)) {
                item.image_path = cached_path;
            }
        }
    } else if (!data.empty()) {
        if (!std::filesystem::exists(cached_path, ec)) {
            std::ofstream out(cached_path, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(data.data()), data.size());
            }
        }
        item.image_path = cached_path;
    }

    if (!data.empty()) {
        size_t thumb_sz = std::min<size_t>(data.size(), 4096);
        item.thumbnail_bytes.assign(data.begin(), data.begin() + thumb_sz);
        item.text_preview = "[图片] " + std::string(mime_type) + " (" + std::to_string(data.size()) + " B)";
    } else {
        uintmax_t fsz = std::filesystem::file_size(item.image_path, ec);
        item.text_preview = "[图片] " + std::string(mime_type) + " (" + std::to_string(fsz) + " B)";
    }
    item.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return add_item(std::move(item));
}

uint64_t ClipboardHistoryManager::add_item(ClipboardHistoryItem item) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Deduplication check by SHA-256
    auto existing_it = std::find_if(items_.begin(), items_.end(), [&](const ClipboardHistoryItem& existing) {
        return existing.sha256 == item.sha256;
    });

    if (existing_it != items_.end()) {
        // Move existing item to top (index 0) and refresh timestamp
        ClipboardHistoryItem existing = std::move(*existing_it);
        items_.erase(existing_it);
        existing.timestamp_ms = item.timestamp_ms;
        existing.origin = item.origin;
        if (!item.full_text.empty()) existing.full_text = std::move(item.full_text);
        if (!item.text_preview.empty()) existing.text_preview = std::move(item.text_preview);
        if (!item.image_path.empty()) existing.image_path = std::move(item.image_path);
        if (!item.thumbnail_bytes.empty()) existing.thumbnail_bytes = std::move(item.thumbnail_bytes);
        uint64_t id = existing.id;
        items_.insert(items_.begin(), std::move(existing));
        trim_to_limit_locked();
        auto_save_locked();
        return id;
    }

    item.id = next_id_++;
    uint64_t new_id = item.id;
    items_.insert(items_.begin(), std::move(item));

    trim_to_limit_locked();
    auto_save_locked();
    return new_id;
}

void ClipboardHistoryManager::set_max_history_limit(size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_limit_ = std::clamp<size_t>(limit, 5, 200);
    trim_to_limit_locked();
    auto_save_locked();
}

size_t ClipboardHistoryManager::max_history_limit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_limit_;
}

size_t ClipboardHistoryManager::item_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

std::vector<ClipboardHistoryItem> ClipboardHistoryManager::get_items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

std::optional<ClipboardHistoryItem> ClipboardHistoryManager::get_item(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : items_) {
        if (item.id == id) {
            return item;
        }
    }
    return std::nullopt;
}

std::optional<ClipboardHistoryItem> ClipboardHistoryManager::get_latest_item() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) return std::nullopt;
    return items_.front();
}

bool ClipboardHistoryManager::delete_item(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(items_.begin(), items_.end(), [id](const ClipboardHistoryItem& item) {
        return item.id == id;
    });
    if (it == items_.end()) {
        return false;
    }
    items_.erase(it);
    auto_save_locked();
    return true;
}

void ClipboardHistoryManager::clear_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    auto_save_locked();
}

bool ClipboardHistoryManager::pin_item(uint64_t id, bool pin) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : items_) {
        if (item.id == id) {
            item.is_pinned = pin;
            auto_save_locked();
            return true;
        }
    }
    return false;
}

std::string ClipboardHistoryManager::mark_selective_paste(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    selective_paste_item_id_ = id;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    selective_paste_token_ = "sel_paste_" + std::to_string(id) + "_" + std::to_string(now_ms);
    selective_paste_time_ = std::chrono::steady_clock::now();
    return selective_paste_token_;
}

bool ClipboardHistoryManager::is_selective_paste_active(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (selective_paste_item_id_ != id || selective_paste_item_id_ == 0) {
        return false;
    }
    auto elapsed = std::chrono::steady_clock::now() - selective_paste_time_;
    return elapsed <= std::chrono::seconds(10);
}

std::optional<uint64_t> ClipboardHistoryManager::active_selective_paste_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (selective_paste_item_id_ == 0) return std::nullopt;
    auto elapsed = std::chrono::steady_clock::now() - selective_paste_time_;
    if (elapsed > std::chrono::seconds(10)) return std::nullopt;
    return selective_paste_item_id_;
}

void ClipboardHistoryManager::clear_selective_paste() {
    std::lock_guard<std::mutex> lock(mutex_);
    selective_paste_item_id_ = 0;
    selective_paste_token_.clear();
}

void ClipboardHistoryManager::trim_to_limit_locked() {
    while (items_.size() > max_limit_) {
        // Evict oldest non-pinned item from back
        auto victim_it = items_.end();
        for (auto it = items_.end(); it != items_.begin();) {
            --it;
            if (!it->is_pinned) {
                victim_it = it;
                break;
            }
        }
        if (victim_it != items_.end()) {
            items_.erase(victim_it);
        } else {
            // All items are pinned, evict the oldest anyway to respect strict limit
            items_.pop_back();
        }
    }
}

void ClipboardHistoryManager::auto_save_locked() {
    if (history_file_path_.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(history_file_path_.parent_path(), ec);

    std::string json = to_json_locked();
    std::filesystem::path tmp_path = history_file_path_.string() + ".tmp";
    std::ofstream out(tmp_path, std::ios::trunc);
    if (out.is_open()) {
        out << json;
        out.close();
        std::filesystem::rename(tmp_path, history_file_path_, ec);
    }
}

bool ClipboardHistoryManager::save_to_disk() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_file_path_.empty()) return false;
    auto_save_locked();
    return true;
}

bool ClipboardHistoryManager::load_from_disk() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_file_path_.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(history_file_path_, ec)) {
        return false;
    }
    std::ifstream in(history_file_path_);
    if (!in.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return from_json_locked(content);
}

std::string ClipboardHistoryManager::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return to_json_locked();
}

std::string ClipboardHistoryManager::to_json_locked() const {
    std::ostringstream ss;
    ss << "[\n";
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& item = items_[i];
        ss << "  {\n";
        ss << "    \"id\": " << item.id << ",\n";
        ss << "    \"type\": " << static_cast<int>(item.type) << ",\n";
        ss << "    \"origin\": " << static_cast<int>(item.origin) << ",\n";
        ss << "    \"mime_type\": \"" << escape_json_string(item.mime_type) << "\",\n";
        ss << "    \"text_preview\": \"" << escape_json_string(item.text_preview) << "\",\n";
        ss << "    \"full_text\": \"" << escape_json_string(item.full_text) << "\",\n";
        ss << "    \"image_path\": \"" << escape_json_string(item.image_path.string()) << "\",\n";
        ss << "    \"thumbnail_hex\": \"" << hex_encode(item.thumbnail_bytes) << "\",\n";
        ss << "    \"sha256\": \"" << to_hex(item.sha256) << "\",\n";
        ss << "    \"timestamp_ms\": " << item.timestamp_ms << ",\n";
        ss << "    \"is_pinned\": " << (item.is_pinned ? "true" : "false") << "\n";
        ss << "  }" << (i + 1 < items_.size() ? "," : "") << "\n";
    }
    ss << "]\n";
    return ss.str();
}

bool ClipboardHistoryManager::from_json(std::string_view json_str) {
    std::lock_guard<std::mutex> lock(mutex_);
    return from_json_locked(json_str);
}

bool ClipboardHistoryManager::from_json_locked(std::string_view json_str) {
    SimpleJsonParser parser(json_str);
    JsonValue root;
    if (!parser.parse(root) || root.type != JsonValue::Type::Array) {
        return false;
    }

    std::vector<ClipboardHistoryItem> loaded;
    uint64_t max_id = 0;

    for (const auto& elem : root.array_val) {
        if (elem.type != JsonValue::Type::Object) continue;
        ClipboardHistoryItem item{};

        if (const auto* v = elem.find("id"); v && v->type == JsonValue::Type::Number) {
            item.id = static_cast<uint64_t>(v->num_val);
            if (item.id > max_id) max_id = item.id;
        }
        if (const auto* v = elem.find("type"); v && v->type == JsonValue::Type::Number) {
            item.type = static_cast<ClipContentType>(v->num_val);
        }
        if (const auto* v = elem.find("origin"); v && v->type == JsonValue::Type::Number) {
            item.origin = static_cast<ClipOrigin>(v->num_val);
        }
        if (const auto* v = elem.find("mime_type"); v && v->type == JsonValue::Type::String) {
            item.mime_type = v->str_val;
        }
        if (const auto* v = elem.find("text_preview"); v && v->type == JsonValue::Type::String) {
            item.text_preview = v->str_val;
        }
        if (const auto* v = elem.find("full_text"); v && v->type == JsonValue::Type::String) {
            item.full_text = v->str_val;
        }
        if (const auto* v = elem.find("image_path"); v && v->type == JsonValue::Type::String) {
            item.image_path = v->str_val;
        }
        if (const auto* v = elem.find("thumbnail_hex"); v && v->type == JsonValue::Type::String) {
            item.thumbnail_bytes = hex_decode(v->str_val);
        }
        if (const auto* v = elem.find("sha256"); v && v->type == JsonValue::Type::String) {
            auto bytes = hex_decode(v->str_val);
            if (bytes.size() == 32) {
                std::copy(bytes.begin(), bytes.end(), item.sha256.begin());
            }
        }
        if (const auto* v = elem.find("timestamp_ms"); v && v->type == JsonValue::Type::Number) {
            item.timestamp_ms = static_cast<int64_t>(v->num_val);
        }
        if (const auto* v = elem.find("is_pinned"); v && v->type == JsonValue::Type::Bool) {
            item.is_pinned = v->bool_val;
        }

        loaded.push_back(std::move(item));
    }

    items_ = std::move(loaded);
    next_id_ = max_id + 1;
    trim_to_limit_locked();
    return true;
}

}  // namespace l2a
