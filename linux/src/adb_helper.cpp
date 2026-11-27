#include "adb_helper.hpp"

#include <array>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace l2a::linux_app {

int AdbHelper::run_cmd_capture(
    const std::string& cmd,
    std::string& out_text,
    int timeout_ms) {
    out_text.clear();
    int pfd[2];
    if (::pipe(pfd) != 0) {
        return -1;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pfd[0]);
        ::close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        // Child process
        ::close(pfd[0]);
        ::dup2(pfd[1], STDOUT_FILENO);
        ::dup2(pfd[1], STDERR_FILENO);
        ::close(pfd[1]);

        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) {
                ::close(devnull);
            }
        }

        ::execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        ::_exit(127);
    }

    // Parent process
    ::close(pfd[1]);

    // LINUX-08: Non-blocking I/O with poll() timeout
    int flags = ::fcntl(pfd[0], F_GETFL, 0);
    ::fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

    auto start_time = std::chrono::steady_clock::now();
    bool timed_out = false;
    std::array<char, 512> buf{};

    while (true) {
        auto now = std::chrono::steady_clock::now();
        int elapsed_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count());
        int remaining_ms = timeout_ms - elapsed_ms;
        if (remaining_ms <= 0) {
            timed_out = true;
            break;
        }

        struct pollfd pfd_poll{};
        pfd_poll.fd = pfd[0];
        pfd_poll.events = POLLIN | POLLHUP | POLLERR;

        int poll_ret = ::poll(&pfd_poll, 1, remaining_ms);
        if (poll_ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (poll_ret == 0) {
            timed_out = true;
            break;
        }

        if (pfd_poll.revents & POLLIN) {
            ssize_t bytes_read = ::read(pfd[0], buf.data(), buf.size());
            if (bytes_read > 0) {
                out_text.append(buf.data(), static_cast<size_t>(bytes_read));
            } else if (bytes_read == 0) {
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        }

        if (pfd_poll.revents & (POLLHUP | POLLERR)) {
            while (true) {
                ssize_t bytes_read = ::read(pfd[0], buf.data(), buf.size());
                if (bytes_read > 0) {
                    out_text.append(buf.data(), static_cast<size_t>(bytes_read));
                } else {
                    break;
                }
            }
            break;
        }
    }

    ::close(pfd[0]);

    if (timed_out) {
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        out_text += "\n[Error: Subprocess timed out after " + std::to_string(timeout_ms) + " ms]";
        return -1;
    }

    int status = 0;
    ::waitpid(pid, &status, 0);

    // LINUX-09: Correct exit code parsing using WIFEXITED / WEXITSTATUS
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

namespace {

std::string resolve_adb_binary() {
    std::error_code ec;
    // 1. Check local workspace tools directory first
    std::filesystem::path local_adb = std::filesystem::current_path() / "tools" / "bin" / "adb";
    if (std::filesystem::exists(local_adb, ec)) {
        return local_adb.string();
    }
    local_adb = std::filesystem::current_path() / "tools" / "platform-tools" / "adb";
    if (std::filesystem::exists(local_adb, ec)) {
        return local_adb.string();
    }

    // 2. Check system PATH
    std::string out;
    if (AdbHelper::run_cmd_capture("which adb", out, 2000) == 0 && !out.empty()) {
        return "adb";
    }

    // 3. Check Android Studio SDK directory
    const char* home = std::getenv("HOME");
    if (home) {
        std::filesystem::path sdk_adb =
            std::filesystem::path(home) / "Android" / "Sdk" / "platform-tools" / "adb";
        if (std::filesystem::exists(sdk_adb, ec)) {
            return sdk_adb.string();
        }
    }
    return {};
}

}  // namespace

bool AdbHelper::is_adb_available() {
    return !resolve_adb_binary().empty();
}

AdbActionResult AdbHelper::setup_android_permissions_and_usb_tunnel(
    uint16_t transfer_port,
    const std::string& package_name) {
    AdbActionResult res{};
    std::string adb_bin = resolve_adb_binary();
    if (adb_bin.empty()) {
        res.success = false;
        res.message = "未检测到 adb 工具。可通过 `sudo apt install android-tools-adb` 安装后再试。";
        return res;
    }

    // 1. Grant overlay & clipboard permissions via AppOps (ignore if OEM ROM uses different op name)
    std::string out_ops;
    run_cmd_capture(adb_bin + " shell cmd appops set " + package_name + " SYSTEM_ALERT_WINDOW allow", out_ops, 5000);
    run_cmd_capture(adb_bin + " shell cmd appops set " + package_name + " READ_CLIPBOARD allow", out_ops, 5000);

    // 2. LINUX-10: Setup bidirectional USB port forwarding:
    // - adb forward tcp:53322 tcp:53321 (Linux 127.0.0.1:53322 -> Phone 53321 where L2AService listens)
    // - adb reverse tcp:53323 tcp:<port> (Phone 127.0.0.1:53323 -> Linux <transfer_port> where l2a-desktop listens)
    std::string out_fwd;
    int rc_fwd = run_cmd_capture(
        adb_bin + " forward tcp:53322 tcp:53321", out_fwd, 5000);

    std::string out_rev;
    int rc_rev = run_cmd_capture(
        adb_bin + " reverse tcp:53323 tcp:" + std::to_string(transfer_port), out_rev, 5000);

    // LINUX-10: Require both forward and reverse commands to succeed
    if (rc_fwd == 0 && rc_rev == 0) {
        res.success = true;
        res.message = "已激活 ADB 权限与双向 USB 高速通道 (PC:53322 -> 手机:53321, 手机:53323 -> PC:" +
                      std::to_string(transfer_port) + ")！";
    } else {
        res.success = false;
        res.message = "ADB 端口转发失败（请确认手机已连接并开启 USB 调试）：" + out_fwd + " " + out_rev;
    }
    return res;
}

}  // namespace l2a::linux_app
