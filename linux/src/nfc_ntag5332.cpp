#include "nfc_ntag5332.hpp"
#include "adb_helper.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/i2c-dev.h>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace l2a::linux_app {

std::string NfcNtag5332Controller::resolve_i2c_dev_path() {
    std::error_code ec;
    std::filesystem::path sysfs_dev = "/sys/bus/i2c/devices/i2c-NTAG5332:00";
    if (std::filesystem::exists(sysfs_dev, ec)) {
        auto canon = std::filesystem::canonical(sysfs_dev, ec);
        if (!ec) {
            for (auto p = canon.parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
                std::string fname = p.filename().string();
                if (fname.rfind("i2c-", 0) == 0 && fname.size() > 4) {
                    bool all_digits = true;
                    for (size_t i = 4; i < fname.size(); ++i) {
                        if (!std::isdigit(static_cast<unsigned char>(fname[i]))) {
                            all_digits = false;
                            break;
                        }
                    }
                    if (all_digits) {
                        return "/dev/" + fname;
                    }
                }
            }
        }
    }
    return "/dev/i2c-0";
}

NfcHardwareStatus NfcNtag5332Controller::probe_hardware() {
    NfcHardwareStatus st{};
    st.sysfs_path = "/sys/bus/i2c/devices/i2c-NTAG5332:00";

    std::error_code ec;
    st.sysfs_present = std::filesystem::exists(st.sysfs_path, ec);

    if (st.sysfs_present) {
        std::ifstream path_in(st.sysfs_path + "/firmware_node/path");
        std::getline(path_in, st.acpi_path);

        std::ifstream status_in(st.sysfs_path + "/firmware_node/status");
        int status_val = 0;
        if (status_in >> status_val) {
            st.acpi_ready = (status_val == 15);
        }
    }

    std::string dev_path = resolve_i2c_dev_path();
    st.i2c_dev_writable = (::access(dev_path.c_str(), R_OK | W_OK) == 0);

    std::ostringstream oss;
    if (st.sysfs_present) {
        oss << "已检测到触控板 NFC 芯片 NTAG5332 ("
            << (st.acpi_path.empty() ? dev_path : st.acpi_path)
            << ", 状态: " << (st.acpi_ready ? "就绪" : "未知")
            << ", 模式: " << (st.i2c_dev_writable ? "RF + I2C 双模就绪 (" + dev_path + ")" : "RF 一碰唤醒就绪")
            << ")";
    } else {
        oss << "未检测到板载 NTAG5332 芯片 (仅使用局域网/USB 发现)";
    }
    st.summary = oss.str();
    return st;
}

std::string NfcNtag5332Controller::build_tap_uri(
    std::string_view host_device_id,
    std::string_view host_name,
    uint16_t tcp_port,
    std::string_view pubkey_fingerprint_hex) {
    std::string fp_short(pubkey_fingerprint_hex.substr(
        0, std::min<size_t>(16, pubkey_fingerprint_hex.size())));
    std::ostringstream oss;
    oss << "l2a://tap?host_id=" << host_device_id
        << "&name=" << host_name
        << "&port=" << tcp_port
        << "&fp=" << fp_short;
    return oss.str();
}

std::vector<uint8_t> NfcNtag5332Controller::encode_type5_ndef_tlv(std::string_view uri) {
    // LINUX-13: Validate URI length to prevent payload length byte integer overflow
    // NDEF Short Record requires payload length < 255.
    // Payload consists of 1-byte identifier (0x00) + uri.size().
    // Therefore 1 + uri.size() <= 254 (i.e. uri.size() <= 253).
    if (uri.empty() || uri.size() > 253) {
        return {};
    }

    uint8_t payload_len = static_cast<uint8_t>(1 + uri.size());
    std::vector<uint8_t> ndef_record;
    ndef_record.reserve(4 + payload_len);
    ndef_record.push_back(0xD1);
    ndef_record.push_back(0x01);
    ndef_record.push_back(payload_len);
    ndef_record.push_back('U');
    ndef_record.push_back(0x00);  // No prefix abbreviation (custom scheme l2a://)
    ndef_record.insert(ndef_record.end(), uri.begin(), uri.end());

    // Type 5 Tag Capability Container (4 bytes) + NDEF TLV (0x03, len, record, 0xFE)
    std::vector<uint8_t> tlv;
    tlv.push_back(0xE1);  // Magic NDEF CC
    tlv.push_back(0x40);  // Version 1.0, Read/Write allowed
    tlv.push_back(0x20);  // Memory size indicator
    tlv.push_back(0x00);  // Additional feature byte

    tlv.push_back(0x03);  // NDEF Message TLV Tag
    tlv.push_back(static_cast<uint8_t>(ndef_record.size()));
    tlv.insert(tlv.end(), ndef_record.begin(), ndef_record.end());
    tlv.push_back(0xFE);  // Terminator TLV
    while (tlv.size() % 4 != 0) {
        tlv.push_back(0x00);  // Pad to 4-byte block boundary
    }
    return tlv;
}

std::vector<uint8_t> NfcNtag5332Controller::encode_type5_ndef_with_aar_tlv(
    std::string_view uri,
    std::string_view android_package) {
    if (uri.empty() || android_package.empty()) {
        return encode_type5_ndef_tlv(uri);
    }

    constexpr std::string_view kAarType = "android.com:pkg";
    // Record 0 (URI): 4 header bytes + 1 prefix byte + uri.size()
    // Record 1 (AAR): 3 header bytes + 15 type bytes + android_package.size()
    size_t rec0_size = 5 + uri.size();
    size_t rec1_size = 3 + kAarType.size() + android_package.size();
    size_t total_ndef_size = rec0_size + rec1_size;

    // Must fit within 1-byte TLV length (< 255)
    if (1 + uri.size() > 254 || android_package.size() > 254 || total_ndef_size > 254) {
        return encode_type5_ndef_tlv(uri);
    }

    std::vector<uint8_t> ndef_msg;
    ndef_msg.reserve(total_ndef_size);

    // Record 0: MB=1, ME=0, CF=0, SR=1, IL=0, TNF=0x01 (Well-Known URI) -> 0x91
    ndef_msg.push_back(0x91);
    ndef_msg.push_back(0x01);
    ndef_msg.push_back(static_cast<uint8_t>(1 + uri.size()));
    ndef_msg.push_back('U');
    ndef_msg.push_back(0x00);
    ndef_msg.insert(ndef_msg.end(), uri.begin(), uri.end());

    // Record 1: MB=0, ME=1, CF=0, SR=1, IL=0, TNF=0x04 (External Type AAR) -> 0x54
    ndef_msg.push_back(0x54);
    ndef_msg.push_back(static_cast<uint8_t>(kAarType.size()));
    ndef_msg.push_back(static_cast<uint8_t>(android_package.size()));
    ndef_msg.insert(ndef_msg.end(), kAarType.begin(), kAarType.end());
    ndef_msg.insert(ndef_msg.end(), android_package.begin(), android_package.end());

    std::vector<uint8_t> tlv;
    tlv.push_back(0xE1);  // Magic NDEF CC
    tlv.push_back(0x40);  // Version 1.0, Read/Write allowed
    tlv.push_back(0x20);  // Memory size indicator (256 bytes)
    tlv.push_back(0x00);  // Additional feature byte

    tlv.push_back(0x03);  // NDEF Message TLV Tag
    tlv.push_back(static_cast<uint8_t>(ndef_msg.size()));
    tlv.insert(tlv.end(), ndef_msg.begin(), ndef_msg.end());
    tlv.push_back(0xFE);  // Terminator TLV
    while (tlv.size() % 4 != 0) {
        tlv.push_back(0x00);
    }
    return tlv;
}

bool NfcNtag5332Controller::try_write_via_i2c(
    std::string_view uri,
    std::string& out_status_message,
    uint8_t i2c_addr,
    bool allow_pkexec) {
    // LINUX-13: Validate URI length
    if (uri.empty() || uri.size() > 253) {
        out_status_message = "错误：NDEF URI 为空或长度超出限制 (< 254 字节)，无法写入。";
        return false;
    }

    auto tlv = encode_type5_ndef_with_aar_tlv(uri, "com.reid.l2a");
    if (tlv.empty()) {
        out_status_message = "错误：NDEF TLV 编码失败。";
        return false;
    }

    // LINUX-12: Dynamic I2C adapter resolution
    std::string dev_path = resolve_i2c_dev_path();
    int fd = ::open(dev_path.c_str(), O_RDWR);
    if (fd < 0 && allow_pkexec && (errno == EACCES || errno == EPERM)) {
        const char* disp = std::getenv("DISPLAY");
        const char* wdisp = std::getenv("WAYLAND_DISPLAY");
        if ((disp && *disp) || (wdisp && *wdisp)) {
            std::string pk_out;
            std::string cmd =
                "pkexec /bin/sh -c 'chmod 0666 /dev/i2c-* && "
                "printf \"KERNEL==\\\"i2c-[0-9]*\\\", MODE=\\\"0666\\\"\\n\" "
                "> /etc/udev/rules.d/99-l2a-ntag5332.rules'";
            if (AdbHelper::run_cmd_capture(cmd, pk_out, 30000) == 0) {
                fd = ::open(dev_path.c_str(), O_RDWR);
            }
        }
    }

    if (fd < 0) {
        out_status_message =
            "提示：当前用户无 " + dev_path +
            " 写权限，已启用手机端 NFC 硬件 UID 绑定与一碰秒传模式（打开手机 App 点击【写入/绑定触控板 NFC】后碰一下触控板即可）。";
        return false;
    }

    struct UniqueFd {
        int fd;
        ~UniqueFd() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    } closer{fd};

    // Candidate I2C addresses for NXP NTAG5 (NTP5332/NTA5332 default is 0x54)
    const std::array<uint8_t, 2> candidate_addrs = {i2c_addr, static_cast<uint8_t>(0x55)};
    uint8_t active_addr = i2c_addr;
    bool bound_slave = false;
    for (uint8_t addr : candidate_addrs) {
        if (::ioctl(fd, I2C_SLAVE, addr) >= 0 || ::ioctl(fd, I2C_SLAVE_FORCE, addr) >= 0) {
            active_addr = addr;
            bound_slave = true;
            break;
        }
    }
    if (!bound_slave) {
        out_status_message = "无法绑定 I2C 从设备地址，已回退至手机端 NFC 模式。";
        return false;
    }

    // NXP NTAG5 (NTP5332) user EEPROM uses 16-bit BLOCK index (0x0000, 0x0001, 0x0002...),
    // where each block holds 4 bytes (NOT byte offset blk * 4).
    const int max_attempts = allow_pkexec ? 20 : 3;
    for (size_t blk = 0; blk < tlv.size() / 4; ++blk) {
        uint16_t block_index = static_cast<uint16_t>(blk);
        uint8_t packet[6] = {
            static_cast<uint8_t>((block_index >> 8U) & 0xFFU),
            static_cast<uint8_t>(block_index & 0xFFU),
            tlv[blk * 4 + 0],
            tlv[blk * 4 + 1],
            tlv[blk * 4 + 2],
            tlv[blk * 4 + 3]
        };

        bool write_ok = false;
        useconds_t backoff_us = 2000;

        for (int attempt = 0; attempt <= max_attempts; ++attempt) {
            ssize_t nw = ::write(fd, packet, sizeof(packet));
            if (nw == static_cast<ssize_t>(sizeof(packet))) {
                write_ok = true;
                break;
            }
            int err = errno;
            if (err == EBUSY || err == EAGAIN || err == ENXIO || err == ETIMEDOUT || err == EREMOTEIO || err == EIO) {
                if (attempt < max_attempts) {
                    ::usleep(backoff_us);
                    if (backoff_us < 100000) {
                        backoff_us *= 2;
                    }
                    continue;
                }
            }
            break;
        }

        if (!write_ok) {
            out_status_message =
                "I2C 写入 NTAG5332 块 #" + std::to_string(blk) +
                " 未应答（触控板芯片可能处于休眠或原厂锁保护），请直接使用手机端 App 点击【写入/绑定触控板 NFC】碰触控板完成绑定！";
            return false;
        }
        ::usleep(5000);  // Wait 5ms for EEPROM block programming
    }

    (void)active_addr;
    out_status_message = "成功通过 " + dev_path + " 将最新一碰传 NDEF (含 AAR) 写入触控板 NTAG5332 芯片！";
    return true;
}

}  // namespace l2a::linux_app
