import com.reid.l2a.CoreBridge;

public class TestJvm {
    public static void main(String[] args) {
        try {
            System.out.println("CoreBridge: " + CoreBridge.INSTANCE);
            System.out.println("CoreBridge isConnected: " + CoreBridge.INSTANCE.isConnected());
            System.out.println("CoreBridge nativeIsConnected: " + CoreBridge.INSTANCE.nativeIsConnected());
        } catch (Throwable t) {
            t.printStackTrace();
        }
    }
}
