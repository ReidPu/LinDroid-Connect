#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

#define private public
#define protected public
#include "wayland_clipboard.hpp"
#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"
#include "l2a/history_manager.hpp"
#undef private
#undef protected

#define CHALLENGER_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[-] [CHALLENGER-FAIL] " << msg << " (" << #cond << ") at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::exit(1); \
        } \
    } while (0)

namespace fs = std::filesystem;
using namespace l2a;
using namespace l2a::linux_app;

namespace {

class ChallengerClipListener : public ICoreListener {
public:
    std::mutex mtx;
    std::condition_variable cv;

    bool connected{false};
    std::string peer_name;
    std::vector<ReceivedClipboardItem> clips;

    void on_connection_state_changed(
        bool is_conn,
        const std::string& /*peer_id*/,
        const std::string& name) override {
        std::lock_guard<std::mutex> lock(mtx);
        connected = is_conn;
        peer_name = name;
        cv.notify_all();
    }

    void on_clipboard_received(const ReceivedClipboardItem& item) override {
        std::lock_guard<std::mutex> lock(mtx);
        clips.push_back(item);
        cv.notify_all();
    }
};

std::vector<uint8_t> generate_png_payload(size_t size) {
    std::vector<uint8_t> data(size);
    const uint8_t png_header[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::memcpy(data.data(), png_header, sizeof(png_header));
    for (size_t i = sizeof(png_header); i < size; ++i) {
        data[i] = static_cast<uint8_t>((i * 13 + 37) % 256);
    }
    return data;
}

}  // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "[CHALLENGER M1-C3] Empirical Test Suite: Image Clipboard & Nautilus Resolution\n";
    std::cout << "=========================================================\n";

    auto scratch_dir = fs::temp_directory_path() / ("l2a_challenger_m1_c3_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(scratch_dir, ec);
    fs::create_directories(scratch_dir, ec);

    // Setup mock binaries directory for wl-copy interception
    fs::path mock_bin = scratch_dir / "mock_bin";
    fs::create_directories(mock_bin, ec);

    fs::path wl_copy_log = scratch_dir / "wl_copy.log";
    fs::path wl_copy_data = scratch_dir / "wl_copy.data";

    // Create mock wl-copy script
    {
        std::ofstream mock_sh(mock_bin / "wl-copy");
        mock_sh << "#!/bin/sh\n";
        mock_sh << "echo \"$@\" >> \"" << wl_copy_log.string() << "\"\n";
        mock_sh << "cat > \"" << wl_copy_data.string() << "\"\n";
        mock_sh << "exit 0\n";
    }
    ::chmod((mock_bin / "wl-copy").c_str(), 0755);

    // Prepend mock_bin to PATH
    std::string orig_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    std::string new_path = mock_bin.string() + ":" + orig_path;
    ::setenv("PATH", new_path.c_str(), 1);

    // =========================================================================
    // SUITE 1: URI Parsing & Image Resolution Variations
    // =========================================================================
    std::cout << "\n--- SUITE 1: URI Parsing & Image Extraction Stress Tests ---\n";

    // 1.1 Create real test files
    auto normal_png = scratch_dir / "sample.png";
    {
        std::ofstream ofs(normal_png, std::ios::binary);
        ofs << "PNG_IMAGE_BYTES_12345";
    }

    auto space_jpg = scratch_dir / "holiday photo 2026.jpg";
    {
        std::ofstream ofs(space_jpg, std::ios::binary);
        ofs << "JPEG_IMAGE_BYTES_54321";
    }

    auto multi_space_jpg = scratch_dir / "my trip to the high mountain pass.jpeg";
    {
        std::ofstream ofs(multi_space_jpg, std::ios::binary);
        ofs << "JPEG_MULTI_SPACE_BYTES";
    }

    // Uppercase extensions
    auto upper_png = scratch_dir / "UPPERCASE_PHOTO.PNG";
    {
        std::ofstream ofs(upper_png, std::ios::binary);
        ofs << "UPPER_PNG_DATA";
    }

    auto upper_jpeg = scratch_dir / "UPPERCASE_PHOTO.JPEG";
    {
        std::ofstream ofs(upper_jpeg, std::ios::binary);
        ofs << "UPPER_JPEG_DATA";
    }

    auto upper_jpg = scratch_dir / "MIXED_EXT.Jpg";
    {
        std::ofstream ofs(upper_jpg, std::ios::binary);
        ofs << "MIXED_JPG_DATA";
    }

    auto upper_webp = scratch_dir / "MODERN_PHOTO.WebP";
    {
        std::ofstream ofs(upper_webp, std::ios::binary);
        ofs << "WEBP_DATA";
    }

    auto upper_gif = scratch_dir / "ANIMATION.GIF";
    {
        std::ofstream ofs(upper_gif, std::ios::binary);
        ofs << "GIF_DATA";
    }

    auto upper_bmp = scratch_dir / "BITMAP_IMAGE.BMP";
    {
        std::ofstream ofs(upper_bmp, std::ios::binary);
        ofs << "BMP_DATA";
    }

    // Special UTF-8 / URL encoded characters
    auto utf8_png = scratch_dir / "测试图片_2026.png";
    {
        std::ofstream ofs(utf8_png, std::ios::binary);
        ofs << "UTF8_IMAGE_DATA";
    }

    // Non-image files
    auto doc_pdf = scratch_dir / "document.pdf";
    {
        std::ofstream ofs(doc_pdf, std::ios::binary);
        ofs << "%PDF-1.4 dummy pdf document";
    }

    auto archive_tar = scratch_dir / "archive.tar.gz";
    {
        std::ofstream ofs(archive_tar, std::ios::binary);
        ofs << "GZ_DUMMY_ARCHIVE";
    }

    auto plain_txt = scratch_dir / "readme.txt";
    {
        std::ofstream ofs(plain_txt);
        ofs << "Plain text content";
    }

    auto empty_png = scratch_dir / "empty_corrupt.png";
    {
        std::ofstream ofs(empty_png, std::ios::binary);
        // zero bytes
    }

    std::cout << "[TEST 1.1] Standard file:/// URI...\n";
    auto res1 = try_resolve_local_image_file("file://" + normal_png.string());
    CHALLENGER_ASSERT(res1.is_image, "Normal file:// URI must resolve as image");
    CHALLENGER_ASSERT(res1.path == normal_png, "Path must match normal_png");
    CHALLENGER_ASSERT(res1.mime_type == "image/png", "MIME type must be image/png");
    std::cout << "  -> PASSED: file://" << normal_png.string() << " resolved to image/png.\n";

    std::cout << "[TEST 1.2] file://localhost/ URI prefix...\n";
    auto res1_lh = try_resolve_local_image_file("file://localhost" + normal_png.string());
    CHALLENGER_ASSERT(res1_lh.is_image, "file://localhost/ prefix must resolve");
    CHALLENGER_ASSERT(res1_lh.path == normal_png, "Path must match normal_png");
    CHALLENGER_ASSERT(res1_lh.mime_type == "image/png", "MIME type must be image/png");
    std::cout << "  -> PASSED: file://localhost prefix resolved correctly.\n";

    std::cout << "[TEST 1.3] Raw absolute path (/path/to/img.png)...\n";
    auto res1_raw = try_resolve_local_image_file(normal_png.string());
    CHALLENGER_ASSERT(res1_raw.is_image, "Raw absolute path must resolve");
    CHALLENGER_ASSERT(res1_raw.path == normal_png, "Path must match normal_png");
    std::cout << "  -> PASSED: Raw absolute path resolved correctly.\n";

    std::cout << "[TEST 1.4] Percent-encoded space (file:///.../holiday%20photo%202026.jpg)...\n";
    std::string uri_space = "file://" + scratch_dir.string() + "/holiday%20photo%202026.jpg";
    auto res_space = try_resolve_local_image_file(uri_space);
    CHALLENGER_ASSERT(res_space.is_image, "Space percent-decoding must resolve");
    CHALLENGER_ASSERT(res_space.path == space_jpg, "Decoded path must match space_jpg");
    CHALLENGER_ASSERT(res_space.mime_type == "image/jpeg", "MIME type must be image/jpeg");
    std::cout << "  -> PASSED: Percent-encoded single space resolved to " << res_space.path << "\n";

    std::cout << "[TEST 1.5] Multiple percent-encoded spaces and .jpeg extension...\n";
    std::string uri_multi_space = "file://" + scratch_dir.string() + "/my%20trip%20to%20the%20high%20mountain%20pass.jpeg";
    auto res_multi = try_resolve_local_image_file(uri_multi_space);
    CHALLENGER_ASSERT(res_multi.is_image, "Multiple spaces must decode cleanly");
    CHALLENGER_ASSERT(res_multi.path == multi_space_jpg, "Path must match multi_space_jpg");
    CHALLENGER_ASSERT(res_multi.mime_type == "image/jpeg", "MIME type must be image/jpeg");
    std::cout << "  -> PASSED: Multiple spaces and .jpeg resolved correctly.\n";

    std::cout << "[TEST 1.6] Uppercase and mixed case extensions (.PNG, .JPEG, .Jpg, .WebP, .GIF, .BMP)...\n";
    auto res_u_png = try_resolve_local_image_file("file://" + upper_png.string());
    CHALLENGER_ASSERT(res_u_png.is_image && res_u_png.mime_type == "image/png", ".PNG must resolve to image/png");

    auto res_u_jpeg = try_resolve_local_image_file("file://" + upper_jpeg.string());
    CHALLENGER_ASSERT(res_u_jpeg.is_image && res_u_jpeg.mime_type == "image/jpeg", ".JPEG must resolve to image/jpeg");

    auto res_u_jpg = try_resolve_local_image_file("file://" + upper_jpg.string());
    CHALLENGER_ASSERT(res_u_jpg.is_image && res_u_jpg.mime_type == "image/jpeg", ".Jpg must resolve to image/jpeg");

    auto res_u_webp = try_resolve_local_image_file("file://" + upper_webp.string());
    CHALLENGER_ASSERT(res_u_webp.is_image && res_u_webp.mime_type == "image/webp", ".WebP must resolve to image/webp");

    auto res_u_gif = try_resolve_local_image_file("file://" + upper_gif.string());
    CHALLENGER_ASSERT(res_u_gif.is_image && res_u_gif.mime_type == "image/gif", ".GIF must resolve to image/gif");

    auto res_u_bmp = try_resolve_local_image_file("file://" + upper_bmp.string());
    CHALLENGER_ASSERT(res_u_bmp.is_image && res_u_bmp.mime_type == "image/bmp", ".BMP must resolve to image/bmp");
    std::cout << "  -> PASSED: All uppercase / mixed-case image extensions handled case-insensitively.\n";

    std::cout << "[TEST 1.7] Nautilus prefixes: copy\\n..., cut\\n..., CRLF, and comment lines...\n";
    // copy \n
    std::string nautilus_copy_lf = "copy\nfile://" + normal_png.string() + "\n";
    auto res_n_copy_lf = try_resolve_local_image_file(nautilus_copy_lf);
    CHALLENGER_ASSERT(res_n_copy_lf.is_image && res_n_copy_lf.path == normal_png, "Nautilus copy\\n must resolve");

    // cut \n
    std::string nautilus_cut_lf = "cut\nfile://" + normal_png.string() + "\n";
    auto res_n_cut_lf = try_resolve_local_image_file(nautilus_cut_lf);
    CHALLENGER_ASSERT(res_n_cut_lf.is_image && res_n_cut_lf.path == normal_png, "Nautilus cut\\n must resolve");

    // copy \r\n
    std::string nautilus_crlf = "copy\r\nfile://" + normal_png.string() + "\r\n";
    auto res_n_crlf = try_resolve_local_image_file(nautilus_crlf);
    CHALLENGER_ASSERT(res_n_crlf.is_image && res_n_crlf.path == normal_png, "Nautilus CRLF must resolve");

    // URI-list comments and surrounding whitespace
    std::string nautilus_comments = "# Nautilus file manager clipboard\r\n# Timestamp: 2026-10-24\r\ncopy\r\n  file://" +
                                    normal_png.string() + "  \r\n";
    auto res_n_comm = try_resolve_local_image_file(nautilus_comments);
    CHALLENGER_ASSERT(res_n_comm.is_image && res_n_comm.path == normal_png, "Comment header and whitespace must resolve");
    std::cout << "  -> PASSED: Nautilus copy, cut, CRLF, and # comments handled cleanly.\n";

    std::cout << "[TEST 1.8] Multiple URIs in text/uri-list...\n";
    // Multiple images: picks first image
    std::string multi_uri_images = "copy\nfile://" + normal_png.string() + "\nfile://" + space_jpg.string() + "\n";
    auto res_multi_img = try_resolve_local_image_file(multi_uri_images);
    CHALLENGER_ASSERT(res_multi_img.is_image && res_multi_img.path == normal_png, "Multi-URI picks first valid entry");

    // Mixed files: non-image first (PDF) -> MUST return is_image == false
    std::string multi_uri_pdf_first = "copy\nfile://" + doc_pdf.string() + "\nfile://" + normal_png.string() + "\n";
    auto res_pdf_first = try_resolve_local_image_file(multi_uri_pdf_first);
    CHALLENGER_ASSERT(!res_pdf_first.is_image, "Multi-URI with non-image first must NOT resolve as image");
    std::cout << "  -> PASSED: Multiple URIs properly prioritized; non-image lead yields is_image=false.\n";

    std::cout << "[TEST 1.9] Non-image URIs (PDF, TXT, TAR.GZ, directories) and fallbacks...\n";
    auto res_pdf = try_resolve_local_image_file("file://" + doc_pdf.string());
    CHALLENGER_ASSERT(!res_pdf.is_image, "PDF URI must not be recognized as image");

    auto res_txt = try_resolve_local_image_file("file://" + plain_txt.string());
    CHALLENGER_ASSERT(!res_txt.is_image, "TXT URI must not be recognized as image");

    auto res_tar = try_resolve_local_image_file("file://" + archive_tar.string());
    CHALLENGER_ASSERT(!res_tar.is_image, "TAR.GZ URI must not be recognized as image");

    auto res_dir = try_resolve_local_image_file("file://" + scratch_dir.string());
    CHALLENGER_ASSERT(!res_dir.is_image, "Directory URI must not be recognized as image");

    auto res_empty = try_resolve_local_image_file("file://" + empty_png.string());
    CHALLENGER_ASSERT(!res_empty.is_image, "Zero-byte image file must be rejected (size == 0)");

    auto res_nonexistent = try_resolve_local_image_file("file:///tmp/definitely_not_existing_photo_99999.png");
    CHALLENGER_ASSERT(!res_nonexistent.is_image, "Nonexistent file URI must be rejected");

    auto res_arbitrary_text = try_resolve_local_image_file("Check out this photo: https://example.com/test.png");
    CHALLENGER_ASSERT(!res_arbitrary_text.is_image, "Arbitrary web text must be rejected");

    auto res_malformed_pct = try_resolve_local_image_file("file:///tmp/%ZZ%5G%invalid.png");
    CHALLENGER_ASSERT(!res_malformed_pct.is_image, "Malformed percent URI must fail gracefully");
    std::cout << "  -> PASSED: All non-image files, zero-byte files, and arbitrary text rejected cleanly.\n";

    // =========================================================================
    // SUITE 2 & 3: Connected Nodes End-to-End Pipeline & Remote Pull
    // =========================================================================
    std::cout << "\n--- SUITE 2 & 3: Connected End-to-End Pipeline & Remote Pull ---\n";

    EngineConfig cfg_node_a{};
    cfg_node_a.storage_dir = scratch_dir / "node_a_storage";
    cfg_node_a.receive_dir = scratch_dir / "node_a_recv";
    cfg_node_a.device_name = "Linux-Host";
    cfg_node_a.platform = "linux";
    cfg_node_a.listen_port = 0;
    cfg_node_a.discovery_port = 0;

    EngineConfig cfg_node_b{};
    cfg_node_b.storage_dir = scratch_dir / "node_b_storage";
    cfg_node_b.receive_dir = scratch_dir / "node_b_recv";
    cfg_node_b.device_name = "Android-Client";
    cfg_node_b.platform = "android";
    cfg_node_b.listen_port = 0;
    cfg_node_b.discovery_port = 0;

    ChallengerClipListener listener_a;
    ChallengerClipListener listener_b;

    Engine node_a(cfg_node_a, &listener_a);
    Engine node_b(cfg_node_b, &listener_b);

    CHALLENGER_ASSERT(node_b.start(), "Node B start failed");
    uint16_t port_b = node_b.bound_tcp_port();
    CHALLENGER_ASSERT(port_b > 0, "Node B TCP port invalid");

    CHALLENGER_ASSERT(node_a.start(), "Node A start failed");
    node_a.register_candidate_peer("127.0.0.1", port_b);
    CHALLENGER_ASSERT(node_a.ensure_connected(), "Node A ensure_connected failed");

    {
        std::unique_lock<std::mutex> lock(listener_a.mtx);
        CHALLENGER_ASSERT(listener_a.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_a.connected;
        }), "Handshake timed out on Node A");
    }
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.connected;
        }), "Handshake timed out on Node B");
    }
    std::cout << "[INFO] Node A and Node B connected over TLS.\n";

    std::vector<std::string> bridge_logs;
    WaylandClipboardBridge bridge(node_a, [&](const std::string& msg) {
        bridge_logs.push_back(msg);
    });

    // Test non-image fallback in on_gtk_text_received with active connection
    std::cout << "[TEST 2.1] Non-image URI fallback to plain text push over TLS...\n";
    size_t count_before = listener_b.clips.size();
    std::string pdf_uri = "file://" + doc_pdf.string();
    WaylandClipboardBridge::on_gtk_text_received(nullptr, pdf_uri.c_str(), &bridge);

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.clips.size() > count_before;
        }), "Timed out waiting for non-image text fallback on Node B");
    }
    const auto& rec_pdf_as_text = listener_b.clips.back();
    CHALLENGER_ASSERT(rec_pdf_as_text.mime_type == "text/plain", "Non-image URI MUST arrive as text/plain");
    std::string rec_pdf_str(reinterpret_cast<const char*>(rec_pdf_as_text.data.data()), rec_pdf_as_text.data.size());
    CHALLENGER_ASSERT(rec_pdf_str == pdf_uri, "Received text must match original URI string exactly");
    std::cout << "  -> PASSED: Non-image URI (" << pdf_uri << ") transferred cleanly as text/plain (NOT binary image).\n";

    // Test image URI resolution in on_gtk_text_received with active connection
    std::cout << "[TEST 2.2] Image URI resolution to push_clipboard_file over TLS...\n";
    count_before = listener_b.clips.size();
    std::string img_uri = "copy\nfile://" + normal_png.string() + "\n";
    WaylandClipboardBridge::on_gtk_text_received(nullptr, img_uri.c_str(), &bridge);

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.clips.size() > count_before;
        }), "Timed out waiting for image file sync on Node B");
    }
    const auto& rec_img = listener_b.clips.back();
    CHALLENGER_ASSERT(rec_img.mime_type == "image/png", "Image URI MUST arrive as image/png");
    auto normal_png_sha = sha256_file_range(normal_png, 0, 0);
    CHALLENGER_ASSERT(normal_png_sha.has_value(), "SHA failed");
    CHALLENGER_ASSERT(sha256_bytes(rec_img.data) == *normal_png_sha, "Binary bytes SHA-256 must match file");
    std::cout << "  -> PASSED: Image URI successfully resolved to binary image/png stream with matching SHA-256.\n";

    // 3.1 Pull when Node A has text
    std::cout << "[TEST 3.1] Pull latest text clipboard (clip_id == 0)...\n";
    std::string test_text = "Important text clipboard #42";
    CHALLENGER_ASSERT(node_a.push_clipboard_text(test_text), "Node A push text failed");

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return !listener_b.clips.empty() && listener_b.clips.back().mime_type == "text/plain";
        }), "Node B failed to receive initial text");
    }
    count_before = listener_b.clips.size();

    // Node B calls pull_remote_clipboard()
    CHALLENGER_ASSERT(node_b.pull_remote_clipboard(), "pull_remote_clipboard failed");
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.clips.size() > count_before;
        }), "Timed out waiting for pulled text");
    }
    const auto& pulled_text_item = listener_b.clips.back();
    CHALLENGER_ASSERT(pulled_text_item.mime_type == "text/plain", "Pulled MIME must be text/plain");
    std::string pulled_str(reinterpret_cast<const char*>(pulled_text_item.data.data()), pulled_text_item.data.size());
    CHALLENGER_ASSERT(pulled_str == test_text, "Pulled text must match original exactly");
    std::cout << "  -> PASSED: Pulled latest text correctly (" << pulled_str << ").\n";

    // 3.2 Pull when Node A has binary image
    std::cout << "[TEST 3.2] Pull latest binary image clipboard (clip_id == 0)...\n";
    auto pull_img_data = generate_png_payload(75 * 1024);
    auto pull_img_sha = sha256_bytes(pull_img_data);
    CHALLENGER_ASSERT(node_a.push_clipboard_bytes("image/png", pull_img_data), "Node A push image failed");

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.clips.back().mime_type == "image/png";
        }), "Node B failed to receive image push");
    }
    count_before = listener_b.clips.size();

    // Node B requests pull
    CHALLENGER_ASSERT(node_b.pull_remote_clipboard(), "pull_remote_clipboard failed");
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.clips.size() > count_before;
        }), "Timed out waiting for pulled image");
    }
    const auto& pulled_img_item = listener_b.clips.back();
    CHALLENGER_ASSERT(pulled_img_item.mime_type == "image/png", "Pulled MIME must be image/png");
    CHALLENGER_ASSERT(sha256_bytes(pulled_img_item.data) == pull_img_sha, "Pulled image SHA-256 must match");
    std::cout << "  -> PASSED: Pulled latest binary image correctly (75 KB, SHA-256 verified).\n";

    // 3.3 Pull when Node A has file-backed image
    std::cout << "[TEST 3.3] Pull latest file-backed image clipboard (clip_id == 0)...\n";
    auto file_backed_jpg = scratch_dir / "sample_file_backed.jpg";
    {
        std::ofstream ofs(file_backed_jpg, std::ios::binary);
        ofs << "DUMMY_JPEG_DATA_FILE_BACKED_PULL_TEST_9988";
    }
    auto file_backed_sha = sha256_file_range(file_backed_jpg, 0, 0);
    CHALLENGER_ASSERT(file_backed_sha.has_value(), "SHA calculation failed");

    CHALLENGER_ASSERT(node_a.push_clipboard_file("image/jpeg", file_backed_jpg), "Push file failed");
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.clips.back().mime_type == "image/jpeg";
        }), "Node B failed to receive file image push");
    }
    count_before = listener_b.clips.size();

    // Node B requests pull
    CHALLENGER_ASSERT(node_b.pull_remote_clipboard(), "pull_remote_clipboard failed");
    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return listener_b.clips.size() > count_before;
        }), "Timed out waiting for pulled file-backed image");
    }
    const auto& pulled_file_item = listener_b.clips.back();
    CHALLENGER_ASSERT(pulled_file_item.mime_type == "image/jpeg", "Pulled MIME must be image/jpeg");
    CHALLENGER_ASSERT(sha256_bytes(pulled_file_item.data) == *file_backed_sha, "Pulled file SHA-256 must match");
    std::cout << "  -> PASSED: Pulled latest file-backed image correctly.\n";

    // =========================================================================
    // SUITE 4: Large Image Binary Streams (>512KB) & Temp File Streaming
    // =========================================================================
    std::cout << "\n--- SUITE 4: Large Image Binary Streams (>512KB) Stress Tests ---\n";

    // 4.1 1.2MB image push via push_clipboard_bytes
    std::cout << "[TEST 4.1] Push 1.2 MB binary image via push_clipboard_bytes...\n";
    size_t large_sz_1 = 1200 * 1024;  // 1.2 MB
    auto large_img_data = generate_png_payload(large_sz_1);
    auto large_img_sha = sha256_bytes(large_img_data);

    count_before = listener_b.clips.size();
    CHALLENGER_ASSERT(node_a.push_clipboard_bytes("image/png", large_img_data), "Node A push 1.2MB failed");

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(10), [&] {
            return listener_b.clips.size() > count_before;
        }), "Timed out receiving 1.2 MB image stream");
    }

    const auto& rec_large_1 = listener_b.clips.back();
    CHALLENGER_ASSERT(rec_large_1.mime_type == "image/png", "MIME must be image/png");
    CHALLENGER_ASSERT(rec_large_1.total_size == large_sz_1, "Total size must match 1.2 MB");
    CHALLENGER_ASSERT(rec_large_1.data.empty(), "In-memory data buffer MUST be empty for >512KB streams");
    CHALLENGER_ASSERT(!rec_large_1.temp_file_path.empty(), "temp_file_path must be populated");
    CHALLENGER_ASSERT(fs::exists(rec_large_1.temp_file_path), "Temp file must exist on disk during callback");

    auto disk_sha = sha256_file_range(rec_large_1.temp_file_path, 0, 0);
    CHALLENGER_ASSERT(disk_sha.has_value() && *disk_sha == large_img_sha, "Streamed temp file SHA-256 must match exactly");
    std::cout << "  -> PASSED: 1.2 MB image streamed via temp file (" << rec_large_1.temp_file_path
              << ") with 100% SHA-256 match.\n";

    // 4.2 Verify apply_remote_clipboard handles large temp file image
    std::cout << "[TEST 4.2] WaylandClipboardBridge::apply_remote_clipboard on large temp file image...\n";
    // Clear mock logs
    {
        std::ofstream ofs(wl_copy_log, std::ios::trunc);
    }
    bridge.apply_remote_clipboard(rec_large_1);

    // Read mock wl_copy_log and check arguments
    std::ifstream wl_log_in(wl_copy_log);
    std::string wl_line;
    std::getline(wl_log_in, wl_line);
    CHALLENGER_ASSERT(wl_line.find("--type image/png") != std::string::npos, "wl-copy must be called with --type image/png");

    // Check size of data piped to wl-copy
    uintmax_t piped_sz = fs::file_size(wl_copy_data, ec);
    CHALLENGER_ASSERT(piped_sz == large_sz_1, "Piped data size to wl-copy must equal 1.2 MB exactly");
    std::cout << "  -> PASSED: Large image written to wl-copy from temp file (" << piped_sz << " bytes piped).\n";

    // 4.3 3.5MB image file push via push_clipboard_file
    std::cout << "[TEST 4.3] Push 3.5 MB image file via push_clipboard_file...\n";
    auto huge_img_path = scratch_dir / "huge_photo_3500k.png";
    {
        std::ofstream ofs(huge_img_path, std::ios::binary);
        const uint8_t png_header[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        ofs.write(reinterpret_cast<const char*>(png_header), sizeof(png_header));
        std::vector<char> chunk(64 * 1024, 'H');
        size_t total_written = sizeof(png_header);
        while (total_written < 3500 * 1024) {
            size_t to_write = std::min<size_t>(chunk.size(), 3500 * 1024 - total_written);
            ofs.write(chunk.data(), to_write);
            total_written += to_write;
        }
    }
    auto huge_sha = sha256_file_range(huge_img_path, 0, 0);
    CHALLENGER_ASSERT(huge_sha.has_value(), "Huge SHA failed");

    count_before = listener_b.clips.size();
    CHALLENGER_ASSERT(node_a.push_clipboard_file("image/png", huge_img_path), "Push 3.5MB file failed");

    {
        std::unique_lock<std::mutex> lock(listener_b.mtx);
        CHALLENGER_ASSERT(listener_b.cv.wait_for(lock, std::chrono::seconds(15), [&] {
            return listener_b.clips.size() > count_before;
        }), "Timed out receiving 3.5 MB image file stream");
    }

    const auto& rec_huge = listener_b.clips.back();
    CHALLENGER_ASSERT(rec_huge.mime_type == "image/png", "MIME must be image/png");
    CHALLENGER_ASSERT(rec_huge.total_size == 3500 * 1024, "Total size must match 3.5 MB");
    CHALLENGER_ASSERT(rec_huge.data.empty(), "In-memory buffer must be empty for 3.5MB stream");
    CHALLENGER_ASSERT(!rec_huge.temp_file_path.empty(), "Temp file path must be populated");

    auto huge_rec_sha = sha256_file_range(rec_huge.temp_file_path, 0, 0);
    CHALLENGER_ASSERT(huge_rec_sha.has_value() && *huge_rec_sha == *huge_sha, "3.5MB file SHA-256 must match exactly");
    std::cout << "  -> PASSED: 3.5 MB image file streamed successfully with matching SHA-256.\n";

    // 4.4 Verify history manager recorded large image item
    std::cout << "[TEST 4.4] Verify ClipboardHistoryManager captures large image stream...\n";
    auto b_items = node_b.history_manager().get_items();
    CHALLENGER_ASSERT(!b_items.empty(), "Node B history must not be empty");
    const auto& top_item = b_items.front();
    CHALLENGER_ASSERT(top_item.type == ClipContentType::Image, "Top item must be Image");
    CHALLENGER_ASSERT(top_item.mime_type == "image/png", "Top item MIME must be image/png");
    CHALLENGER_ASSERT(top_item.text_preview.find("3584000 B") != std::string::npos, "Preview must show byte size");
    std::cout << "  -> PASSED: History manager correctly captured large streamed image.\n";

    // Stop nodes
    node_a.stop();
    node_b.stop();

    fs::remove_all(scratch_dir, ec);

    std::cout << "\n=========================================================\n";
    std::cout << "[CHALLENGER M1-C3] ALL EMPIRICAL STRESS TESTS PASSED (100%)!\n";
    std::cout << "=========================================================\n";
    return 0;
}
