/*
 * ChgFcc — reproduces OplusChargerHelper.getPsyBatteryFcc() from a root shell.
 *
 * The charger service is resolved and the method is called through reflection:
 *
 *   1) AIDL  vendor.oplus.hardware.charger.ICharger/default
 *            ICharger$Stub.asInterface(ServiceManager.getService(...))
 *   2) HIDL  vendor.oplus.hardware.charger.V1_0.ICharger.getService()
 *   -> getPsyBatteryFcc(),  -1 on any failure
 *
 * No android.* types are referenced at compile time, so this builds with a
 * plain JDK and is then converted to DEX:
 *
 *   javac --release 8 -d out src/ChgFcc.java
 *   d8 --release --lib <android.jar> --output out out/ChgFcc.class
 *
 * Run on device:
 *   CLASSPATH=<dex>:<charger java jars> app_process /system/bin ChgFcc
 */

public class ChgFcc {

    private static final String AIDL_NAME = "vendor.oplus.hardware.charger.ICharger/default";

    public static void main(String[] args) {
        int fcc = -1;
        String via = "none";

        /* ---- 1) AIDL ------------------------------------------------ */
        try {
            Class<?> binderCls = Class.forName("android.os.IBinder");
            Class<?> smCls = Class.forName("android.os.ServiceManager");
            Object binder = smCls.getMethod("getService", String.class)
                                 .invoke(null, AIDL_NAME);
            if (binder != null) {
                Class<?> stub = Class.forName("vendor.oplus.hardware.charger.ICharger$Stub");
                Object svc = stub.getMethod("asInterface", binderCls).invoke(null, binder);
                if (svc != null) {
                    Class<?> iface = Class.forName("vendor.oplus.hardware.charger.ICharger");
                    Object r = iface.getMethod("getPsyBatteryFcc").invoke(svc);
                    if (r instanceof Integer) {
                        fcc = ((Integer) r).intValue();
                        via = "aidl";
                    }
                }
            }
        } catch (Throwable t) {
            System.out.println("aidl_err=" + t.getClass().getSimpleName());
        }

        /* ---- 2) HIDL ------------------------------------------------ */
        if ("none".equals(via)) {
            try {
                Class<?> iface = Class.forName("vendor.oplus.hardware.charger.V1_0.ICharger");
                Object svc = iface.getMethod("getService").invoke(null);
                if (svc != null) {
                    Object r = iface.getMethod("getPsyBatteryFcc").invoke(svc);
                    if (r instanceof Integer) {
                        fcc = ((Integer) r).intValue();
                        via = "hidl";
                    }
                }
            } catch (Throwable t) {
                System.out.println("hidl_err=" + t.getClass().getSimpleName());
            }
        }

        System.out.println("fcc=" + fcc);
        System.out.println("via=" + via);
    }
}
