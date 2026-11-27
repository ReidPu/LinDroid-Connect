#include "l2a/engine.hpp"
#include "l2a/sha256.hpp"

#include <jni.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#define LOG_TAG "L2A_JNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#else
#define LOGE(...) std::fprintf(stderr, "[L2A_JNI ERROR] " __VA_ARGS__), std::fprintf(stderr, "\n")
#define LOGI(...) std::printf("[L2A_JNI INFO] " __VA_ARGS__), std::printf("\n")
#endif

namespace {

JavaVM* g_jvm = nullptr;
jclass g_core_bridge_class = nullptr;

// Cached method IDs (ANDROID-13)
jmethodID g_mid_onPeerDiscovered = nullptr;
jmethodID g_mid_onConnectionChanged = nullptr;
jmethodID g_mid_onClipboardReceived = nullptr;
jmethodID g_mid_onClipboardPullRequested = nullptr;
jmethodID g_mid_onFileCompleted = nullptr;
jmethodID g_mid_onFileProgress = nullptr;

// Callback object and lifecycle mutex (ANDROID-02)
jobject g_callback_obj = nullptr;
std::mutex g_callback_mtx;

// Engine reference and mutex (ANDROID-03)
std::shared_ptr<l2a::Engine> g_engine;
std::mutex g_engine_mtx;

std::filesystem::path g_storage_dir;

// Scoped Storage FD streams tracking (ANDROID-05)
struct ActiveFdStream {
    uint32_t file_index{0};
    int fd{-1};
    std::filesystem::path link_path;
    std::filesystem::path dir_path;
};
std::mutex g_fd_streams_mtx;
std::unordered_map<uint64_t, std::vector<ActiveFdStream>> g_active_fd_streams;

// Automatic pthread detachment mechanism (ANDROID-01)
pthread_key_t g_jni_detach_key;
pthread_once_t g_jni_detach_once = PTHREAD_ONCE_INIT;

void jni_detach_thread_destructor(void* /*val*/) {
    if (g_jvm) {
        g_jvm->DetachCurrentThread();
    }
}

void jni_create_detach_key() {
    pthread_key_create(&g_jni_detach_key, jni_detach_thread_destructor);
}

JNIEnv* attach_env() {
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr;
    jint get_res = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (get_res == JNI_OK && env) {
        return env;
    }

#if defined(__ANDROID__)
    jint res = g_jvm->AttachCurrentThread(&env, nullptr);
#else
    jint res = g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr);
#endif

    if (res == JNI_OK && env) {
        pthread_once(&g_jni_detach_once, jni_create_detach_key);
        if (pthread_getspecific(g_jni_detach_key) == nullptr) {
            pthread_setspecific(g_jni_detach_key, reinterpret_cast<void*>(1));
        }
        return env;
    }
    return nullptr;
}

// Exception check and clear guard (ANDROID-04)
bool check_and_clear_exception(JNIEnv* env, const char* method_name) {
    if (env && env->ExceptionCheck()) {
        LOGE("Pending exception occurred after JNI call to %s", method_name);
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
    return false;
}

std::string jstring_to_utf8(JNIEnv* env, jstring jstr) {
    if (!jstr) return {};
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    std::string out(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(jstr, chars);
    return out;
}

std::string sanitize_display_name(std::string name) {
    if (name.empty()) name = "shared_file";
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    return name;
}

class JniCoreListener : public l2a::ICoreListener {
public:
    void on_peer_discovered(const l2a::DiscoveredPeer& peer) override {
        JNIEnv* env = attach_env();
        if (!env) return;

        jobject cb_obj = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_callback_mtx);
            if (!g_callback_obj) return;
            cb_obj = env->NewLocalRef(g_callback_obj);
        }
        if (!cb_obj) return;

        if (g_mid_onPeerDiscovered) {
            jstring jid = env->NewStringUTF(peer.device_id.c_str());
            jstring jname = env->NewStringUTF(peer.device_name.c_str());
            jstring jip = env->NewStringUTF(peer.ip_address.c_str());
            env->CallVoidMethod(
                cb_obj, g_mid_onPeerDiscovered, jid, jname, jip, static_cast<jint>(peer.tcp_port));
            check_and_clear_exception(env, "onPeerDiscovered");
            env->DeleteLocalRef(jid);
            env->DeleteLocalRef(jname);
            env->DeleteLocalRef(jip);
        }
        env->DeleteLocalRef(cb_obj);
    }

    void on_connection_state_changed(
        bool connected,
        const std::string& peer_id,
        const std::string& peer_name) override {
        JNIEnv* env = attach_env();
        if (!env) return;

        jobject cb_obj = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_callback_mtx);
            if (!g_callback_obj) return;
            cb_obj = env->NewLocalRef(g_callback_obj);
        }
        if (!cb_obj) return;

        if (g_mid_onConnectionChanged) {
            jstring jid = env->NewStringUTF(peer_id.c_str());
            jstring jname = env->NewStringUTF(peer_name.c_str());
            env->CallVoidMethod(
                cb_obj, g_mid_onConnectionChanged, connected ? JNI_TRUE : JNI_FALSE, jid, jname);
            check_and_clear_exception(env, "onConnectionChanged");
            env->DeleteLocalRef(jid);
            env->DeleteLocalRef(jname);
        }
        env->DeleteLocalRef(cb_obj);
    }

    void on_clipboard_received(const l2a::ReceivedClipboardItem& item) override {
        JNIEnv* env = attach_env();
        if (!env) return;

        jobject cb_obj = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_callback_mtx);
            if (!g_callback_obj) return;
            cb_obj = env->NewLocalRef(g_callback_obj);
        }
        if (!cb_obj) return;

        if (g_mid_onClipboardReceived) {
            jstring jmime = env->NewStringUTF(item.mime_type.c_str());
            jbyteArray jbytes = env->NewByteArray(static_cast<jsize>(item.data.size()));
            if (!item.data.empty()) {
                env->SetByteArrayRegion(
                    jbytes, 0, static_cast<jsize>(item.data.size()),
                    reinterpret_cast<const jbyte*>(item.data.data()));
            }
            jstring jtmp = env->NewStringUTF(item.temp_file_path.string().c_str());
            env->CallVoidMethod(
                cb_obj,
                g_mid_onClipboardReceived,
                jmime,
                jbytes,
                jtmp,
                item.is_pull_response ? JNI_TRUE : JNI_FALSE);
            check_and_clear_exception(env, "onClipboardReceived");
            env->DeleteLocalRef(jmime);
            env->DeleteLocalRef(jbytes);
            env->DeleteLocalRef(jtmp);
        }
        env->DeleteLocalRef(cb_obj);
    }

    void on_clipboard_lazy_offer(const l2a::ClipOfferPayload& offer) override {
        std::shared_ptr<l2a::Engine> engine;
        {
            std::lock_guard<std::mutex> lock(g_engine_mtx);
            engine = g_engine;
        }
        if (engine) {
            engine->request_clipboard_lazy_pull(offer.clip_id);
        }
    }

    bool on_clipboard_pull_requested_sync() override {
        JNIEnv* env = attach_env();
        if (!env) return false;

        jobject cb_obj = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_callback_mtx);
            if (!g_callback_obj) return false;
            cb_obj = env->NewLocalRef(g_callback_obj);
        }
        if (!cb_obj) return false;

        bool ok = false;
        if (g_mid_onClipboardPullRequested) {
            jboolean res = env->CallBooleanMethod(cb_obj, g_mid_onClipboardPullRequested);
            if (!check_and_clear_exception(env, "onClipboardPullRequested")) {
                ok = (res == JNI_TRUE);
            }
        }
        env->DeleteLocalRef(cb_obj);
        return ok;
    }

    void on_file_progress(const l2a::TransferProgress& progress) override {
        JNIEnv* env = attach_env();
        if (!env) return;

        jobject cb_obj = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_callback_mtx);
            if (!g_callback_obj) return;
            cb_obj = env->NewLocalRef(g_callback_obj);
        }
        if (!cb_obj) return;

        if (g_mid_onFileProgress) {
            env->CallVoidMethod(
                cb_obj,
                g_mid_onFileProgress,
                static_cast<jlong>(progress.bytes_transferred),
                static_cast<jlong>(progress.total_file_bytes),
                progress.is_sending ? JNI_TRUE : JNI_FALSE);
            check_and_clear_exception(env, "onFileProgress");
        }
        env->DeleteLocalRef(cb_obj);
    }

    void on_file_transfer_completed(
        uint64_t batch_id,
        uint32_t file_index,
        const std::filesystem::path& saved_path,
        bool is_sending,
        bool success,
        const std::string& error_msg) override {
        if (is_sending) {
            std::lock_guard<std::mutex> lock(g_fd_streams_mtx);
            auto it = g_active_fd_streams.find(batch_id);
            if (it != g_active_fd_streams.end()) {
                for (auto& s : it->second) {
                    if (s.file_index == file_index || s.link_path == saved_path) {
                        if (s.fd >= 0) {
                            ::close(s.fd);
                            s.fd = -1;
                        }
                        std::error_code ec;
                        std::filesystem::remove(s.link_path, ec);
                        std::filesystem::remove_all(s.dir_path, ec);
                    }
                }
                bool all_done = true;
                for (const auto& s : it->second) {
                    if (s.fd >= 0) {
                        all_done = false;
                        break;
                    }
                }
                if (all_done) {
                    g_active_fd_streams.erase(it);
                }
            }
        }

        JNIEnv* env = attach_env();
        if (!env) return;

        jobject cb_obj = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_callback_mtx);
            if (!g_callback_obj) return;
            cb_obj = env->NewLocalRef(g_callback_obj);
        }
        if (!cb_obj) return;

        if (g_mid_onFileCompleted) {
            jstring jpath = env->NewStringUTF(saved_path.string().c_str());
            jstring jerr = env->NewStringUTF(error_msg.c_str());
            env->CallVoidMethod(
                cb_obj,
                g_mid_onFileCompleted,
                static_cast<jlong>(batch_id),
                static_cast<jint>(file_index),
                jpath,
                is_sending ? JNI_TRUE : JNI_FALSE,
                success ? JNI_TRUE : JNI_FALSE,
                jerr);
            check_and_clear_exception(env, "onFileCompleted");
            env->DeleteLocalRef(jpath);
            env->DeleteLocalRef(jerr);
        }
        env->DeleteLocalRef(cb_obj);
    }
};

JniCoreListener g_listener;

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_jvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    jclass local_cls = env->FindClass("com/reid/l2a/CoreBridge");
    if (!local_cls) {
        LOGE("Failed to find class com/reid/l2a/CoreBridge");
        return JNI_ERR;
    }
    g_core_bridge_class = reinterpret_cast<jclass>(env->NewGlobalRef(local_cls));
    env->DeleteLocalRef(local_cls);

    // Cache method IDs (ANDROID-13)
    g_mid_onPeerDiscovered = env->GetMethodID(
        g_core_bridge_class, "onPeerDiscovered", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
    g_mid_onConnectionChanged = env->GetMethodID(
        g_core_bridge_class, "onConnectionChanged", "(ZLjava/lang/String;Ljava/lang/String;)V");
    g_mid_onClipboardReceived = env->GetMethodID(
        g_core_bridge_class, "onClipboardReceived", "(Ljava/lang/String;[BLjava/lang/String;Z)V");
    g_mid_onClipboardPullRequested = env->GetMethodID(
        g_core_bridge_class, "onClipboardPullRequested", "()Z");
    g_mid_onFileCompleted = env->GetMethodID(
        g_core_bridge_class, "onFileCompleted", "(JILjava/lang/String;ZZLjava/lang/String;)V");
    g_mid_onFileProgress = env->GetMethodID(
        g_core_bridge_class, "onFileProgress", "(JJZ)V");

    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* /*reserved*/) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env) {
        if (g_core_bridge_class) {
            env->DeleteGlobalRef(g_core_bridge_class);
            g_core_bridge_class = nullptr;
        }
    }
}

JNIEXPORT jboolean JNICALL
Java_com_reid_l2a_CoreBridge_nativeInit(
    JNIEnv* env,
    jobject thiz,
    jstring storage_dir,
    jstring receive_dir,
    jstring device_name) {
    std::shared_ptr<l2a::Engine> engine_to_start;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        {
            std::lock_guard<std::mutex> cb_lock(g_callback_mtx);
            if (g_callback_obj) {
                env->DeleteGlobalRef(g_callback_obj);
            }
            g_callback_obj = env->NewGlobalRef(thiz);
        }

        if (g_engine) {
            return JNI_TRUE;
        }

        l2a::EngineConfig cfg{};
        cfg.storage_dir = jstring_to_utf8(env, storage_dir);
        cfg.receive_dir = jstring_to_utf8(env, receive_dir);
        cfg.device_name = jstring_to_utf8(env, device_name);
        cfg.platform = "android";
        cfg.listen_port = l2a::kDefaultTransferPort;
        cfg.discovery_port = l2a::kDefaultDiscoveryPort;

        g_storage_dir = cfg.storage_dir;

        g_engine = std::make_shared<l2a::Engine>(cfg, &g_listener);
        engine_to_start = g_engine;
    }

    return (engine_to_start && engine_to_start->start()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_reid_l2a_CoreBridge_nativeStop(JNIEnv* env, jobject /*thiz*/) {
    std::shared_ptr<l2a::Engine> engine_to_stop;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine_to_stop = std::move(g_engine);
    }

    if (engine_to_stop) {
        engine_to_stop->stop();
        engine_to_stop.reset();
    }

    // Clean up any remaining active FD streams (ANDROID-05)
    {
        std::lock_guard<std::mutex> lock(g_fd_streams_mtx);
        for (auto& [bid, streams] : g_active_fd_streams) {
            for (auto& s : streams) {
                if (s.fd >= 0) {
                    ::close(s.fd);
                    s.fd = -1;
                }
                std::error_code ec;
                std::filesystem::remove(s.link_path, ec);
                std::filesystem::remove_all(s.dir_path, ec);
            }
        }
        g_active_fd_streams.clear();
    }

    // Safely delete callback object under lock (ANDROID-02)
    jobject old_cb = nullptr;
    {
        std::lock_guard<std::mutex> cb_lock(g_callback_mtx);
        old_cb = g_callback_obj;
        g_callback_obj = nullptr;
    }
    if (old_cb) {
        env->DeleteGlobalRef(old_cb);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_reid_l2a_CoreBridge_nativeConnect(
    JNIEnv* env,
    jobject /*thiz*/,
    jstring ip,
    jint port) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine) return JNI_FALSE;

    std::string ip_str = jstring_to_utf8(env, ip);
    uint16_t port_u16 = static_cast<uint16_t>(port);
    engine->register_candidate_peer(ip_str, port_u16);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return JNI_FALSE;

    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port_u16);
    if (::inet_pton(AF_INET, ip_str.c_str(), &dest.sin_addr) != 1) {
        ::close(fd);
        return JNI_FALSE;
    }

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    if (rc < 0) {
        if (errno == EINPROGRESS) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int prc = ::poll(&pfd, 1, 3000);
            if (prc <= 0) {
                ::close(fd);
                return JNI_FALSE;
            }
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                ::close(fd);
                return JNI_FALSE;
            }
        } else {
            ::close(fd);
            return JNI_FALSE;
        }
    }

    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags);
    }

    return engine->connect_with_fd(fd, /*is_client=*/true) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_reid_l2a_CoreBridge_nativeIsConnected(
    JNIEnv* /*env*/,
    jobject /*thiz*/) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    return (engine && engine->is_connected()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_reid_l2a_CoreBridge_nativePushClipboardTextEx(
    JNIEnv* env,
    jobject /*thiz*/,
    jstring text,
    jboolean bypass_loopback,
    jboolean is_pull_response) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine) return JNI_FALSE;
    return engine->push_clipboard_text(
               jstring_to_utf8(env, text),
               bypass_loopback == JNI_TRUE,
               is_pull_response == JNI_TRUE)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_reid_l2a_CoreBridge_nativePushClipboardText(
    JNIEnv* env,
    jobject thiz,
    jstring text) {
    return Java_com_reid_l2a_CoreBridge_nativePushClipboardTextEx(
        env, thiz, text, JNI_FALSE, JNI_FALSE);
}

JNIEXPORT jboolean JNICALL
Java_com_reid_l2a_CoreBridge_nativePushClipboardBytesEx(
    JNIEnv* env,
    jobject /*thiz*/,
    jstring mime_type,
    jbyteArray data,
    jboolean bypass_loopback,
    jboolean is_pull_response) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine || !data) return JNI_FALSE;

    std::string mime = jstring_to_utf8(env, mime_type);
    if (mime.empty()) {
        mime = "application/octet-stream";
    }

    jsize len = env->GetArrayLength(data);
    std::vector<uint8_t> buffer(len);
    if (len > 0) {
        env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(buffer.data()));
    }

    return engine->push_clipboard_bytes(
               mime,
               buffer,
               false,
               bypass_loopback == JNI_TRUE,
               is_pull_response == JNI_TRUE)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_reid_l2a_CoreBridge_nativePushClipboardBytes(
    JNIEnv* env,
    jobject thiz,
    jstring mime_type,
    jbyteArray data) {
    return Java_com_reid_l2a_CoreBridge_nativePushClipboardBytesEx(
        env, thiz, mime_type, data, JNI_FALSE, JNI_FALSE);
}

JNIEXPORT jlong JNICALL
Java_com_reid_l2a_CoreBridge_nativeSendFile(
    JNIEnv* env,
    jobject /*thiz*/,
    jstring file_path) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine) return 0;
    std::vector<std::filesystem::path> paths = {jstring_to_utf8(env, file_path)};
    return static_cast<jlong>(engine->send_files(paths));
}

JNIEXPORT jlong JNICALL
Java_com_reid_l2a_CoreBridge_nativeSendFileFd(
    JNIEnv* env,
    jobject /*thiz*/,
    jint fd,
    jstring display_name,
    jlong /*file_size*/) {
    if (fd < 0) return 0;

    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine) {
        ::close(fd);
        return 0;
    }

    std::string name = sanitize_display_name(jstring_to_utf8(env, display_name));

    static std::atomic<uint64_t> s_stream_seq{1};
    uint64_t seq = s_stream_seq.fetch_add(1);
    std::filesystem::path base = g_storage_dir.empty() ? std::filesystem::path("/tmp") : g_storage_dir;
    std::filesystem::path dir_path = base / "fd_streams" / (std::to_string(fd) + "_" + std::to_string(seq));
    std::error_code ec;
    std::filesystem::create_directories(dir_path, ec);

    std::filesystem::path link_path = dir_path / name;
    std::string proc_fd = "/proc/self/fd/" + std::to_string(fd);
    if (::symlink(proc_fd.c_str(), link_path.c_str()) != 0) {
        LOGE("Failed to create symlink for fd %d: %s", fd, link_path.c_str());
        std::filesystem::remove_all(dir_path, ec);
        ::close(fd);
        return 0;
    }

    std::vector<std::filesystem::path> paths = {link_path};
    uint64_t batch_id = engine->send_files(paths);
    if (batch_id == 0) {
        std::filesystem::remove_all(dir_path, ec);
        ::close(fd);
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_fd_streams_mtx);
        g_active_fd_streams[batch_id].push_back({0, fd, link_path, dir_path});
    }

    return static_cast<jlong>(batch_id);
}

JNIEXPORT jlong JNICALL
Java_com_reid_l2a_CoreBridge_nativeSendFilesBatch(
    JNIEnv* env,
    jobject /*thiz*/,
    jintArray fds_arr,
    jobjectArray display_names_arr,
    jobjectArray fallback_paths_arr) {
    if (!fds_arr || !display_names_arr || !fallback_paths_arr) return 0;

    jsize count = env->GetArrayLength(fds_arr);
    std::vector<jint> fds(static_cast<size_t>(count));
    if (count > 0) {
        env->GetIntArrayRegion(fds_arr, 0, count, fds.data());
    }

    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine) {
        for (jint fd : fds) {
            if (fd >= 0) ::close(fd);
        }
        return 0;
    }

    static std::atomic<uint64_t> s_batch_seq{1};
    std::filesystem::path base = g_storage_dir.empty() ? std::filesystem::path("/tmp") : g_storage_dir;

    std::vector<std::filesystem::path> paths;
    std::vector<ActiveFdStream> created_streams;
    uint32_t next_file_idx = 0;

    for (jsize i = 0; i < count; ++i) {
        jint fd = fds[static_cast<size_t>(i)];
        auto jname = static_cast<jstring>(env->GetObjectArrayElement(display_names_arr, i));
        auto jfallback = static_cast<jstring>(env->GetObjectArrayElement(fallback_paths_arr, i));
        std::string name = sanitize_display_name(jstring_to_utf8(env, jname));
        std::string fallback = jstring_to_utf8(env, jfallback);
        if (jname) env->DeleteLocalRef(jname);
        if (jfallback) env->DeleteLocalRef(jfallback);

        if (fd >= 0) {
            uint64_t seq = s_batch_seq.fetch_add(1);
            std::filesystem::path dir_path =
                base / "fd_streams" / (std::to_string(fd) + "_" + std::to_string(seq));
            std::error_code ec;
            std::filesystem::create_directories(dir_path, ec);
            std::filesystem::path link_path = dir_path / name;
            std::string proc_fd = "/proc/self/fd/" + std::to_string(fd);
            if (::symlink(proc_fd.c_str(), link_path.c_str()) == 0 &&
                std::filesystem::is_regular_file(link_path, ec)) {
                paths.push_back(link_path);
                created_streams.push_back({next_file_idx++, fd, link_path, dir_path});
            } else {
                std::filesystem::remove_all(dir_path, ec);
                ::close(fd);
            }
        } else if (!fallback.empty()) {
            std::filesystem::path fb_path(fallback);
            std::error_code ec;
            if (std::filesystem::is_regular_file(fb_path, ec)) {
                paths.push_back(fb_path);
                next_file_idx++;
            }
        }
    }

    if (paths.empty()) {
        for (auto& s : created_streams) {
            if (s.fd >= 0) ::close(s.fd);
            std::error_code ec;
            std::filesystem::remove_all(s.dir_path, ec);
        }
        return 0;
    }

    uint64_t batch_id = engine->send_files(paths);
    if (batch_id == 0) {
        for (auto& s : created_streams) {
            if (s.fd >= 0) ::close(s.fd);
            std::error_code ec;
            std::filesystem::remove_all(s.dir_path, ec);
        }
        return 0;
    }

    if (!created_streams.empty()) {
        std::lock_guard<std::mutex> lock(g_fd_streams_mtx);
        g_active_fd_streams[batch_id] = std::move(created_streams);
    }

    return static_cast<jlong>(batch_id);
}

JNIEXPORT jboolean JNICALL
Java_com_reid_l2a_CoreBridge_nativePullRemoteClipboard(
    JNIEnv* /*env*/,
    jobject /*thiz*/) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine) return JNI_FALSE;
    return engine->pull_remote_clipboard() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_reid_l2a_CoreBridge_nativeSetMaxHistoryLimit(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jint limit) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine) return;
    engine->history_manager().set_max_history_limit(static_cast<size_t>(limit));
}

JNIEXPORT jstring JNICALL
Java_com_reid_l2a_CoreBridge_nativeGetClipboardHistoryJson(
    JNIEnv* env,
    jobject /*thiz*/) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine) return env->NewStringUTF("[]");
    return env->NewStringUTF(engine->history_manager().to_json().c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_reid_l2a_CoreBridge_nativeDeleteHistoryItem(
    JNIEnv* env,
    jobject /*thiz*/,
    jstring id_str) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine || !id_str) return JNI_FALSE;
    std::string id = jstring_to_utf8(env, id_str);
    try {
        uint64_t id_val = std::stoull(id);
        return engine->history_manager().delete_item(id_val) ? JNI_TRUE : JNI_FALSE;
    } catch (...) {
        return JNI_FALSE;
    }
}

JNIEXPORT void JNICALL
Java_com_reid_l2a_CoreBridge_nativeClearHistory(
    JNIEnv* /*env*/,
    jobject /*thiz*/) {
    std::shared_ptr<l2a::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_engine_mtx);
        engine = g_engine;
    }
    if (!engine) return;
    engine->history_manager().clear_all();
}

}  // extern "C"
