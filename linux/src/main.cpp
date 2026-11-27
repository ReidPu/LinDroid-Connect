#include "desktop_entry.hpp"
#include "gtk_abi.hpp"
#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"
#include "nfc_ntag5332.hpp"
#include "settings_manager.hpp"
#include "wayland_clipboard.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void handle_signal(int /*sig*/) {
    g_running.store(false);
}

class HeadlessDaemon : public l2a::ICoreListener {
public:
    explicit HeadlessDaemon(l2a::EngineConfig config)
        : config_(std::move(config)),
          engine_(config_, this),
          settings_mgr_(config_.storage_dir.empty() ? std::filesystem::path{} : config_.storage_dir) {
        engine_.history_manager().set_max_history_limit(settings_mgr_.history_limit());
        clipboard_bridge_ = std::make_unique<l2a::linux_app::WaylandClipboardBridge>(
            engine_,
            [](const std::string& msg) { std::cout << msg << "\n"; });
        clipboard_bridge_->set_pc_to_phone_auto_share(settings_mgr_.pc_to_phone_auto_share());
        clipboard_bridge_->set_phone_to_pc_auto_share(settings_mgr_.phone_to_pc_auto_share());
    }

    ~HeadlessDaemon() override {
        engine_.stop();
    }

    void start(bool has_display) {
        if (has_display && clipboard_bridge_) {
            clipboard_bridge_->start_gtk_monitoring();
        }
        engine_.start();
        std::cout << "[L2A] Headless service started. Fingerprint: "
                  << l2a::to_hex(engine_.identity().fingerprint).substr(0, 16) << "...\n";
    }

    void on_peer_discovered(const l2a::DiscoveredPeer& peer) override {
        std::cout << "[发现对端] " << peer.device_name << " (" << peer.ip_address << ":"
                  << peer.tcp_port << ", " << peer.platform << ")\n";
    }

    void on_connection_state_changed(
        bool connected,
        const std::string& /*peer_id*/,
        const std::string& peer_name) override {
        std::cout << (connected ? ("[连接成功] " + peer_name) : "[连接断开]") << "\n";
    }

    void on_clipboard_received(const l2a::ReceivedClipboardItem& item) override {
        if (clipboard_bridge_) {
            clipboard_bridge_->apply_remote_clipboard(item);
        }
    }

    void on_clipboard_lazy_offer(const l2a::ClipOfferPayload& offer) override {
        engine_.request_clipboard_lazy_pull(offer.clip_id);
    }

    bool on_clipboard_pull_requested_sync() override {
        if (clipboard_bridge_) {
            return clipboard_bridge_->push_current_clipboard(/*is_pull_response=*/true);
        }
        return false;
    }

    void on_file_progress(const l2a::TransferProgress& /*progress*/) override {}

    void on_file_transfer_completed(
        uint64_t batch_id,
        uint32_t file_index,
        const std::filesystem::path& saved_path,
        bool is_sending,
        bool success,
        const std::string& error_msg) override {
        const char* tag = is_sending ? "文件发送" : "文件接收";
        if (success) {
            std::cout << "[" << tag << "完成] #" << batch_id << "/" << file_index
                      << " -> " << saved_path.string() << "\n";
        } else {
            std::cerr << "[" << tag << "失败] #" << batch_id << "/" << file_index
                      << ": " << error_msg << "\n";
        }
    }

private:
    l2a::EngineConfig config_;
    l2a::Engine engine_;
    l2a::linux_app::SettingsManager settings_mgr_;
    std::unique_ptr<l2a::linux_app::WaylandClipboardBridge> clipboard_bridge_;
};

}  // namespace

int main(int argc, char** argv) {
    // LINUX-03: Ignore SIGPIPE globally to prevent process termination on broken pipes/sockets
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, handle_signal);
    ::signal(SIGTERM, handle_signal);

    const char* home_env = std::getenv("HOME");
    std::filesystem::path home_dir = home_env ? home_env : "/tmp";

    // REAL-04: Auto-install desktop entry for GNOME notification grouping
    l2a::linux_app::ensure_desktop_entry_installed(home_dir);

    // Prepend workspace ./tools/bin to PATH for portable execution
    std::error_code ec;
    auto tools_bin = std::filesystem::current_path() / "tools" / "bin";
    if (std::filesystem::exists(tools_bin, ec)) {
        const char* old_path = std::getenv("PATH");
        std::string new_path = tools_bin.string() + (old_path ? (":" + std::string(old_path)) : "");
        ::setenv("PATH", new_path.c_str(), 1);
    }

    l2a::EngineConfig config{};
    config.storage_dir = home_dir / ".config" / "linux_to_android";
    config.receive_dir = home_dir / "Downloads" / "LinuxToAndroid";
    config.device_name = "Redmi-G-Pro-2024";
    config.platform = "linux";
    config.listen_port = l2a::kDefaultTransferPort;
    config.discovery_port = l2a::kDefaultDiscoveryPort;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--status" || arg == "--nfc-status") {
            l2a::DeviceIdentity id = l2a::DeviceIdentity::load_or_create(
                config.storage_dir, config.device_name);
            auto nfc = l2a::linux_app::NfcNtag5332Controller::probe_hardware();
            std::string uri = l2a::linux_app::NfcNtag5332Controller::build_tap_uri(
                id.device_id, id.device_name, config.listen_port, l2a::to_hex(id.fingerprint));
            auto tlv = l2a::linux_app::NfcNtag5332Controller::encode_type5_ndef_tlv(uri);

            std::cout << "=== Linux ⇄ Android (C++20 Core) Status ===\n"
                      << "Device ID         : " << id.device_id << "\n"
                      << "Device Name       : " << id.device_name << "\n"
                      << "Pubkey Fingerprint: " << l2a::to_hex(id.fingerprint) << "\n"
                      << "Receive Directory : " << config.receive_dir.string() << "\n"
                      << "NFC Hardware      : " << nfc.summary << "\n"
                      << "NFC Tap DeepLink  : " << uri << "\n"
                      << "Type5 NDEF TLV Hex: " << l2a::to_hex(tlv) << "\n";
            return 0;
        }
    }

    const bool has_display = (gtk_init_check(&argc, &argv) != 0);
    HeadlessDaemon daemon(config);
    daemon.start(has_display);

    if (has_display) {
        g_timeout_add(200, [](gpointer) -> gboolean {
            if (!g_running.load()) {
                gtk_main_quit();
                return FALSE_VAL;
            }
            return TRUE_VAL;
        }, nullptr);
        gtk_main();
    } else {
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    return 0;
}
