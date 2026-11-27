cmake_minimum_required(VERSION 3.20)

# Baseline Unit & Loopback Integration Tests
add_executable(test_protocol test_protocol.cpp)
target_compile_options(test_protocol PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_protocol PRIVATE l2a_core)
add_test(NAME ProtocolAndCryptoTest COMMAND test_protocol)

add_executable(test_loopback_transfer test_loopback_transfer.cpp)
target_compile_options(test_loopback_transfer PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_loopback_transfer PRIVATE l2a_core)
add_test(NAME LoopbackTransferAndResumeTest COMMAND test_loopback_transfer)

# TEST-01: Bidirectional transfer concurrency without deadlock
add_executable(test_bidirectional_concurrency test_bidirectional_concurrency.cpp)
target_compile_options(test_bidirectional_concurrency PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_bidirectional_concurrency PRIVATE l2a_core)
add_test(NAME Test_BidirectionalConcurrency COMMAND test_bidirectional_concurrency)

# TEST-02: Large batch metadata transfer (1,000+ files slicing under kMaxFramePayload)
add_executable(test_large_batch_meta test_large_batch_meta.cpp)
target_compile_options(test_large_batch_meta PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_large_batch_meta PRIVATE l2a_core)
add_test(NAME Test_LargeBatchMetadata COMMAND test_large_batch_meta)

# TEST-03: Re-entrant callback deadlock verification
add_executable(test_reentrant_callback test_reentrant_callback.cpp)
target_compile_options(test_reentrant_callback PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_reentrant_callback PRIVATE l2a_core)
add_test(NAME Test_ReentrantCallback COMMAND test_reentrant_callback)

# TEST-04: Simulated abrupt disconnect & reconnect resume test
add_executable(test_disconnect_and_resume test_disconnect_and_resume.cpp)
target_compile_options(test_disconnect_and_resume PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_disconnect_and_resume PRIVATE l2a_core)
add_test(NAME Test_DisconnectAndResume COMMAND test_disconnect_and_resume)

# TEST-05: Corrupted partial file recovery test
add_executable(test_corrupt_recovery test_corrupt_recovery.cpp)
target_compile_options(test_corrupt_recovery PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_corrupt_recovery PRIVATE l2a_core)
add_test(NAME Test_CorruptRecovery COMMAND test_corrupt_recovery)

# TEST-06: Malformed packet fuzzing and DoS boundary defense test
add_executable(test_fuzz_malformed test_fuzz_malformed.cpp)
target_compile_options(test_fuzz_malformed PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_fuzz_malformed PRIVATE l2a_core)
add_test(NAME Test_MalformedPacketFuzz COMMAND test_fuzz_malformed)

# TEST-07: Temporary file cleanup and leak verification test
add_executable(test_temp_cleanup test_temp_cleanup.cpp)
target_compile_options(test_temp_cleanup PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_temp_cleanup PRIVATE l2a_core)
add_test(NAME Test_TempCleanup COMMAND test_temp_cleanup)

# REAL-12: Computer -> Phone initial clipboard sync without prior file transfer
add_executable(test_initial_clipboard_sync test_initial_clipboard_sync.cpp)
target_compile_options(test_initial_clipboard_sync PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_initial_clipboard_sync PRIVATE l2a_core)
add_test(NAME Test_InitialClipboardSync COMMAND test_initial_clipboard_sync)

# REAL-13: Reverse transfer & dual candidate fallback
add_executable(test_reverse_transfer_and_fallback test_reverse_transfer_and_fallback.cpp)
target_compile_options(test_reverse_transfer_and_fallback PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_reverse_transfer_and_fallback PRIVATE l2a_core)
add_test(NAME Test_ReverseTransferAndFallback COMMAND test_reverse_transfer_and_fallback)

# TEST-01 (M1-C3): SettingsManager & ClipboardHistoryManager verification
add_executable(test_settings_and_history
    test_settings_and_history.cpp
    ${CMAKE_SOURCE_DIR}/linux/src/settings_manager.cpp
)
target_include_directories(test_settings_and_history PRIVATE
    ${CMAKE_SOURCE_DIR}/linux/src
)
target_compile_options(test_settings_and_history PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_settings_and_history PRIVATE l2a_core)
add_test(NAME Test_SettingsAndHistory COMMAND test_settings_and_history)

# TEST-02 (M1-C3): Image Clipboard Sync, Remote Pull, Anti-loopback
add_executable(test_image_clipboard_sync test_image_clipboard_sync.cpp)
target_compile_options(test_image_clipboard_sync PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_image_clipboard_sync PRIVATE l2a_core)
add_test(NAME Test_ImageClipboardSync COMMAND test_image_clipboard_sync)

# CHALLENGER (M1-C3): Empirical stress test harness
add_executable(test_challenger_harness
    test_challenger_harness.cpp
    ${CMAKE_SOURCE_DIR}/linux/src/settings_manager.cpp
)
target_include_directories(test_challenger_harness PRIVATE
    ${CMAKE_SOURCE_DIR}/linux/src
)
target_compile_options(test_challenger_harness PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_challenger_harness PRIVATE l2a_core)
add_test(NAME Test_ChallengerHarness COMMAND test_challenger_harness)

add_executable(test_clipboard_and_transfer_logic test_clipboard_and_transfer_logic.cpp)
target_compile_options(test_clipboard_and_transfer_logic PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_clipboard_and_transfer_logic PRIVATE l2a_core)
add_test(NAME Test_ClipboardAndTransferLogic COMMAND test_clipboard_and_transfer_logic)


