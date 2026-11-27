import com.reid.l2a.CoreBridge;
import java.io.*;
import java.nio.file.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public class TestEmpiricalVerification {

    private static Process startPeerServer(int port) throws Exception {
        ProcessBuilder pb = new ProcessBuilder("./build_jni/peer_server", String.valueOf(port));
        pb.redirectErrorStream(true);
        Process p = pb.start();
        BufferedReader reader = new BufferedReader(new InputStreamReader(p.getInputStream()));
        String line = reader.readLine();
        if (line == null || !line.contains("PEER_LISTENING_ON_" + port)) {
            throw new RuntimeException("Peer server failed to start on port " + port + ": " + line);
        }
        System.out.println("  [PEER SERVER] Started on port " + port);
        return p;
    }

    private static void stopPeerServer(Process p) throws Exception {
        if (p != null && p.isAlive()) {
            p.getOutputStream().write("exit\n".getBytes());
            p.getOutputStream().flush();
            p.waitFor();
            System.out.println("  [PEER SERVER] Stopped successfully.");
        }
    }

    public static void main(String[] args) {
        int passed = 0;
        int failed = 0;

        System.out.println("=========================================================");
        System.out.println("EMPIRICAL CHALLENGER VERIFICATION SUITE — M2-C2");
        System.out.println("=========================================================");

        CoreBridge bridge = CoreBridge.INSTANCE;
        Process peer53323 = null;
        Process peerLan = null;

        try {
            // -----------------------------------------------------------------
            // TEST 1: nativeIsConnected initial state & lifecycle
            // -----------------------------------------------------------------
            System.out.println("\n--- TEST 1: nativeIsConnected initial state ---");
            boolean beforeInit = bridge.nativeIsConnected();
            if (!beforeInit) {
                System.out.println("PASS: nativeIsConnected() is false before nativeInit()");
                passed++;
            } else {
                System.err.println("FAIL: nativeIsConnected() was true before nativeInit()");
                failed++;
            }

            // Initialize CoreBridge
            String storageDir = "/tmp/l2a_test_client_storage";
            String recvDir = "/tmp/l2a_test_client_recv";
            new File(storageDir).mkdirs();
            new File(recvDir).mkdirs();
            boolean initOk = bridge.nativeInit(storageDir, recvDir, "TestAndroidClient");
            if (initOk) {
                System.out.println("PASS: nativeInit() succeeded");
                passed++;
            } else {
                System.err.println("FAIL: nativeInit() failed");
                failed++;
            }

            boolean afterInit = bridge.nativeIsConnected();
            if (!afterInit) {
                System.out.println("PASS: nativeIsConnected() is false after nativeInit() with no active peer");
                passed++;
            } else {
                System.err.println("FAIL: nativeIsConnected() was true after nativeInit()");
                failed++;
            }

            // Start peer server on 53323 (simulating Linux PC reachable via ADB reverse tunnel)
            peer53323 = startPeerServer(53323);
            Thread.sleep(150);

            // -----------------------------------------------------------------
            // TEST 2: nativeIsConnected reflects active TLS connection
            // -----------------------------------------------------------------
            System.out.println("\n--- TEST 2: nativeIsConnected active connection state transitions ---");
            boolean connected = bridge.nativeConnect("127.0.0.1", 53323);
            if (connected) {
                System.out.println("PASS: nativeConnect(127.0.0.1, 53323) returned true");
                passed++;
            } else {
                System.err.println("FAIL: nativeConnect(127.0.0.1, 53323) returned false");
                failed++;
            }

            boolean isConn = bridge.nativeIsConnected();
            if (isConn) {
                System.out.println("PASS: nativeIsConnected() accurately reflects active connected state (true)");
                passed++;
            } else {
                System.err.println("FAIL: nativeIsConnected() returned false despite active connection");
                failed++;
            }

            bridge.setConnected(true);

            // -----------------------------------------------------------------
            // TEST 3: CoreBridge.ensureConnected Pre-connected Fast Path
            // -----------------------------------------------------------------
            System.out.println("\n--- TEST 3: CoreBridge.ensureConnected pre-connected fast path ---");
            long t0 = System.currentTimeMillis();
            boolean fastConnected = bridge.ensureConnected(null, 3000);
            long fastDuration = System.currentTimeMillis() - t0;

            if (fastConnected && fastDuration < 50) {
                System.out.println("PASS: ensureConnected returned immediately (" + fastDuration + " ms) when already connected");
                passed++;
            } else {
                System.err.println("FAIL: ensureConnected took " + fastDuration + " ms, result: " + fastConnected);
                failed++;
            }

            // -----------------------------------------------------------------
            // TEST 4: CoreBridge.ensureConnected Primary LAN IP Priority
            // -----------------------------------------------------------------
            System.out.println("\n--- TEST 4: CoreBridge.ensureConnected Primary LAN IP Priority ---");
            peerLan = startPeerServer(54321); // LAN peer
            Thread.sleep(150);

            bridge.setConnected(false);
            bridge.setLastPeerIp("127.0.0.1");
            bridge.setLastPeerPort(54321);

            boolean lanConnected = bridge.ensureConnected(null, 3000);
            if (lanConnected && bridge.isConnected() && bridge.nativeIsConnected()) {
                System.out.println("PASS: ensureConnected successfully connected via primary LAN IP (54321)");
                passed++;
            } else {
                System.err.println("FAIL: ensureConnected failed on primary LAN IP");
                failed++;
            }

            // Stop LAN peer to simulate mobile moving out of Wi-Fi range
            stopPeerServer(peerLan);
            peerLan = null;
            Thread.sleep(200);

            // -----------------------------------------------------------------
            // TEST 5: CoreBridge.ensureConnected Fallback to USB ADB Reverse Port 53323
            // -----------------------------------------------------------------
            System.out.println("\n--- TEST 5: CoreBridge.ensureConnected Fallback to USB ADB reverse port 53323 ---");
            bridge.setConnected(false);
            // LAN port 54321 is now closed (offline)
            bridge.setLastPeerIp("127.0.0.1");
            bridge.setLastPeerPort(54321);

            long tStart = System.currentTimeMillis();
            boolean fallbackOk = bridge.ensureConnected(null, 3000);
            long elapsed = System.currentTimeMillis() - tStart;

            if (fallbackOk && bridge.isConnected() && bridge.nativeIsConnected()) {
                System.out.println("PASS: ensureConnected fell back to ADB reverse port 53323 when LAN IP failed (elapsed: " + elapsed + " ms)");
                passed++;
            } else {
                System.err.println("FAIL: ensureConnected failed to fall back to 53323");
                failed++;
            }

            // -----------------------------------------------------------------
            // TEST 6: Edge Case — Malformed LAN IP gracefully falls back to ADB port 53323
            // -----------------------------------------------------------------
            System.out.println("\n--- TEST 6: Malformed LAN IP gracefully falls back to ADB reverse port 53323 ---");
            bridge.setConnected(false);
            bridge.setLastPeerIp("not.a.valid.ip.999");
            bridge.setLastPeerPort(54321);

            boolean malformedFallbackOk = bridge.ensureConnected(null, 3000);
            if (malformedFallbackOk && bridge.isConnected() && bridge.nativeIsConnected()) {
                System.out.println("PASS: Malformed LAN IP did not crash; gracefully fell back to ADB 53323");
                passed++;
            } else {
                System.err.println("FAIL: Failed to fall back to ADB 53323 on malformed LAN IP");
                failed++;
            }

            // -----------------------------------------------------------------
            // TEST 7: Stress Test — Multi-threaded Concurrent ensureConnected Invocations
            // -----------------------------------------------------------------
            System.out.println("\n--- TEST 7: Multi-threaded Concurrent ensureConnected Invocations ---");
            int numThreads = 8;
            ExecutorService executor = Executors.newFixedThreadPool(numThreads);
            CountDownLatch latch = new CountDownLatch(1);
            AtomicInteger successCount = new AtomicInteger(0);
            AtomicInteger errorCount = new AtomicInteger(0);

            for (int i = 0; i < numThreads; i++) {
                executor.submit(() -> {
                    try {
                        latch.await();
                        boolean res = bridge.ensureConnected(null, 3000);
                        if (res) successCount.incrementAndGet();
                        else errorCount.incrementAndGet();
                    } catch (Exception e) {
                        errorCount.incrementAndGet();
                    }
                });
            }

            latch.countDown();
            executor.shutdown();
            boolean finishedInTime = executor.awaitTermination(5, TimeUnit.SECONDS);

            if (finishedInTime && successCount.get() == numThreads && errorCount.get() == 0) {
                System.out.println("PASS: All " + numThreads + " concurrent threads completed ensureConnected successfully without deadlocks");
                passed++;
            } else {
                System.err.println("FAIL: Concurrent stress test failed: successes=" + successCount.get() + ", errors=" + errorCount.get());
                failed++;
            }

            // -----------------------------------------------------------------
            // TEST 8: CoreBridge.ensureConnected Timeout Behavior
            // -----------------------------------------------------------------
            System.out.println("\n--- TEST 8: CoreBridge.ensureConnected Timeout Behavior (Neither LAN nor USB available) ---");
            // Stop the USB ADB reverse peer so NO endpoints are active
            stopPeerServer(peer53323);
            peer53323 = null;
            Thread.sleep(200);

            bridge.setConnected(false);
            bridge.setLastPeerIp("127.0.0.1");
            bridge.setLastPeerPort(59999); // Dead port

            long timeoutLimitMs = 1200;
            long tTimeoutStart = System.currentTimeMillis();
            boolean timeoutResult = bridge.ensureConnected(null, timeoutLimitMs);
            long timeoutElapsed = System.currentTimeMillis() - tTimeoutStart;

            if (!timeoutResult && !bridge.isConnected() && !bridge.nativeIsConnected()) {
                if (timeoutElapsed >= timeoutLimitMs && timeoutElapsed < timeoutLimitMs + 1000) {
                    System.out.println("PASS: ensureConnected timed out gracefully (elapsed: " + timeoutElapsed + " ms, timeout: " + timeoutLimitMs + " ms) and returned false");
                    passed++;
                } else {
                    System.err.println("FAIL: ensureConnected returned false but timing unexpected: " + timeoutElapsed + " ms");
                    failed++;
                }
            } else {
                System.err.println("FAIL: ensureConnected unexpectedly succeeded when no endpoints were active!");
                failed++;
            }

            // Verify disconnection & stop lifecycle
            bridge.nativeStop();
            boolean isConnAfterStop = bridge.nativeIsConnected();
            if (!isConnAfterStop) {
                System.out.println("PASS: nativeIsConnected() is false after nativeStop()");
                passed++;
            } else {
                System.err.println("FAIL: nativeIsConnected() true after nativeStop()");
                failed++;
            }

            // -----------------------------------------------------------------
            // TEST 9: Reverse Port Configuration Check (53323 vs 53321)
            // -----------------------------------------------------------------
            System.out.println("\n--- TEST 9: Reverse Port Configuration Check (53323 vs 53321) ---");
            String serviceCode = Files.readString(Paths.get("android/app/src/main/java/com/reid/l2a/L2AService.kt"));
            String coreBridgeCode = Files.readString(Paths.get("android/app/src/main/java/com/reid/l2a/CoreBridge.kt"));

            boolean serviceUses53323 = serviceCode.contains("CoreBridge.nativeConnect(\"127.0.0.1\", 53323)");
            boolean serviceAvoids53321On127 = !serviceCode.contains("CoreBridge.nativeConnect(\"127.0.0.1\", 53321)");
            boolean bridgeUses53323Fallback = coreBridgeCode.contains("nativeConnect(\"127.0.0.1\", 53323)");

            if (serviceUses53323 && serviceAvoids53321On127 && bridgeUses53323Fallback) {
                System.out.println("PASS: L2AService.kt correctly connects to ADB reverse port 53323 (NOT 53321)");
                System.out.println("PASS: CoreBridge.kt fallback correctly connects to ADB reverse port 53323");
                passed++;
            } else {
                System.err.println("FAIL: Port mismatch in L2AService or CoreBridge: "
                    + "serviceUses53323=" + serviceUses53323
                    + ", serviceAvoids53321=" + serviceAvoids53321On127
                    + ", bridgeUses53323=" + bridgeUses53323Fallback);
                failed++;
            }

        } catch (Throwable t) {
            System.err.println("FATAL: Unexpected exception in verification suite: " + t);
            t.printStackTrace();
            failed++;
        } finally {
            try { stopPeerServer(peerLan); } catch (Exception ignored) {}
            try { stopPeerServer(peer53323); } catch (Exception ignored) {}
        }

        System.out.println("\n=========================================================");
        System.out.println("TEST SUMMARY: " + passed + " PASSED, " + failed + " FAILED");
        System.out.println("=========================================================");

        if (failed > 0) {
            System.exit(1);
        }
    }
}
