#pragma once

#include <cstdint>
#include <string>

namespace l2a::linux_app {

struct AdbActionResult {
    bool success{false};
    std::string message;
};

class AdbHelper {
public:
    // Checks if `adb` binary is installed and a device is connected
    static bool is_adb_available();

    // Grants Android background clipboard access (READ_CLIPBOARD_IN_BACKGROUND)
    // and sets up bidirectional USB port forwarding:
    // - PC 127.0.0.1:53322 -> Phone 53321
    // - Phone 127.0.0.1:53323 -> PC transfer_port
    static AdbActionResult setup_android_permissions_and_usb_tunnel(
        uint16_t transfer_port,
        const std::string& package_name = "com.reid.l2a");

    // LINUX-08 & LINUX-09: Subprocess runner with non-blocking pipe, poll() timeout,
    // and POSIX WIFEXITED / WEXITSTATUS evaluation.
    static int run_cmd_capture(
        const std::string& cmd,
        std::string& out_text,
        int timeout_ms = 5000);
};

}  // namespace l2a::linux_app
