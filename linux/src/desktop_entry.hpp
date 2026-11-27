#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace l2a::linux_app {

inline void ensure_desktop_entry_installed(const std::filesystem::path& home_dir) {
    std::filesystem::path app_dir = home_dir / ".local" / "share" / "applications";
    std::error_code ec;
    std::filesystem::create_directories(app_dir, ec);
    if (ec) {
        return;
    }

    std::filesystem::path target_desktop = app_dir / "l2a-desktop.desktop";

    // Detect current running executable path via /proc/self/exe
    std::filesystem::path exe_path = "l2a-desktop";
    std::error_code exe_ec;
    auto self_exe = std::filesystem::read_symlink("/proc/self/exe", exe_ec);
    if (!exe_ec && !self_exe.empty()) {
        exe_path = self_exe;
    }

    std::string content =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Linux ⇄ Android 跨端互传\n"
        "Comment=基于 C++20 的 Linux 与 Android 文件互传、剪贴板共享与触控板 NFC 互联工具\n"
        "Exec=" + exe_path.string() + "\n"
        "Icon=network-transmit-receive\n"
        "Terminal=false\n"
        "Categories=Utility;Network;FileTransfer;\n"
        "StartupNotify=false\n";

    std::ofstream ofs(target_desktop);
    if (ofs.is_open()) {
        ofs << content;
        ofs.close();
        ::chmod(target_desktop.c_str(), 0644);
    }
}

inline std::vector<std::string> build_desktop_notification_args(
    const std::string& title,
    const std::string& body) {
    return {
        "notify-send",
        "-a",
        "Linux ⇄ Android",
        "-i",
        "network-transmit-receive",
        "-h",
        "string:desktop-entry:l2a-desktop",
        title,
        body
    };
}

}  // namespace l2a::linux_app
