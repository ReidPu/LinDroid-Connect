#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace l2a::linux_app {

struct NfcHardwareStatus {
    bool sysfs_present{false};
    bool acpi_ready{false};
    bool i2c_dev_writable{false};
    std::string sysfs_path;
    std::string acpi_path;
    std::string summary;
};

class NfcNtag5332Controller {
public:
    static NfcHardwareStatus probe_hardware();

    // Generates the custom DeepLink URI to be written into NTAG5332 (Mode 1 RF / Mode 2 I2C)
    static std::string build_tap_uri(
        std::string_view host_device_id,
        std::string_view host_name,
        uint16_t tcp_port,
        std::string_view pubkey_fingerprint_hex);

    // Encodes an NFC Forum Type 5 Tag NDEF TLV message containing the URI record
    static std::vector<uint8_t> encode_type5_ndef_tlv(std::string_view uri);

    // Encodes an NFC Forum Type 5 Tag NDEF TLV message containing both the URI record
    // and an Android Application Record (AAR: android.com:pkg) so Android routes taps
    // directly to com.reid.l2a without system chooser prompts.
    static std::vector<uint8_t> encode_type5_ndef_with_aar_tlv(
        std::string_view uri,
        std::string_view android_package = "com.reid.l2a");

    // LINUX-12: Dynamically resolves I2C adapter dev node (/dev/i2c-X) from sysfs
    static std::string resolve_i2c_dev_path();

    // Mode 2 (Optional): Attempts to write the NDEF TLV directly via resolved I2C device
    // with exponential backoff retry on transient bus errors (EBUSY, EAGAIN).
    static bool try_write_via_i2c(
        std::string_view uri,
        std::string& out_status_message,
        uint8_t i2c_addr = 0x54,
        bool allow_pkexec = false);
};

}  // namespace l2a::linux_app
