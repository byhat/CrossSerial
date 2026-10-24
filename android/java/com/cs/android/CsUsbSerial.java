package com.cs.android;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.util.Log;

import com.hoho.android.usbserial.driver.UsbSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
import com.hoho.android.usbserial.driver.UsbSerialProber;
import com.hoho.android.usbserial.util.SerialInputOutputManager;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * JNI bridge between the native cs::SerialPort (Android backend) and the
 * usb-serial-for-android driver library. All native-visible methods are
 * static; the native side drives.
 *
 * Data arrival: a {@link SerialInputOutputManager} runs on its own thread and
 * calls {@link #nativeOnData(long, byte[])} which dispatches back into C++.
 *
 * open() status codes returned to native code:
 *   0 = ok, 1 = device not found, 2 = USB permission not granted, 3 = open failed
 */
public class CsUsbSerial {
    private static final String TAG = "crossserial";
    private static final String ACTION_USB_PERMISSION = "com.cs.android.USB_PERMISSION";

    /** Set from native code right after the DEX is loaded. */
    private static volatile Context sContext;

    private static final class Entry {
        UsbSerialPort port;
        UsbDeviceConnection connection;
        SerialInputOutputManager io;
    }
    private static final ConcurrentHashMap<Long, Entry> sOpen = new ConcurrentHashMap<>();

    // ---- native callbacks (implemented in SerialPortAndroid.cpp) ------------
    public static native void nativeOnData(long handle, byte[] data);
    public static native void nativeOnError(long handle, int code);

    private CsUsbSerial() {}

    public static void setContext(Context ctx) {
        sContext = ctx.getApplicationContext();
    }

    // ---- enumeration ---------------------------------------------------------

    public static CsPortInfo[] enumerate() {
        List<CsPortInfo> out = new ArrayList<>();
        try {
            Context ctx = sContext;
            if (ctx == null) { Log.w(TAG, "enumerate: no context (initAndroid not called)"); return new CsPortInfo[0]; }
            UsbManager mgr = (UsbManager) ctx.getSystemService(Context.USB_SERVICE);
            if (mgr == null) { Log.w(TAG, "enumerate: no UsbManager service"); return new CsPortInfo[0]; }

            List<UsbSerialDriver> drivers = UsbSerialProber.getDefaultProber().findAllDrivers(mgr);
            for (UsbSerialDriver d : drivers) {
                UsbDevice dev = d.getDevice();
                CsPortInfo info = new CsPortInfo();
                info.name = dev.getDeviceName();
                info.systemLocation = info.name;
                info.description = d.toString();
                info.manufacturer = safeManufacturer(dev);
                info.serialNumber = safeSerial(mgr, dev);
                info.vid = dev.getVendorId();
                info.pid = dev.getProductId();
                out.add(info);
            }
            Log.i(TAG, "enumerate: usbDevices=" + mgr.getDeviceList().size()
                    + " serialDrivers=" + drivers.size() + " -> " + out.size() + " ports");
        } catch (Exception ex) {
            Log.e(TAG, "enumerate: failed", ex);
        }
        return out.toArray(new CsPortInfo[0]);
    }

    // ---- open / write / close ------------------------------------------------

    public static int open(long handle, String deviceName, int baud,
                           int dataBits, int parity, int stopBits,
                           @SuppressWarnings("unused") int flowControl,
                           boolean requestPermission) {
        try {
            Context ctx = sContext;
            if (ctx == null) return 3;
            UsbManager mgr = (UsbManager) ctx.getSystemService(Context.USB_SERVICE);
            UsbSerialDriver driver = findDriver(mgr, deviceName);
            if (driver == null) return 1;

            UsbDevice dev = driver.getDevice();
            if (!mgr.hasPermission(dev)) {
                // Either ask (blocking, with the system dialog) or bail out
                // immediately - the native side makes that choice via
                // cs::SerialPort::setPermissionRequestEnabled().
                if (!requestPermission || !requestPermission(mgr, ctx, dev)) return 2;
            }
            UsbDeviceConnection conn = mgr.openDevice(dev);
            if (conn == null) return 3;

            UsbSerialPort port = driver.getPorts().get(0);
            port.open(conn);
            port.setParameters(baud, dataBits, mapStopBits(stopBits), mapParity(parity));

            Entry e = new Entry();
            e.port = port;
            e.connection = conn;
            e.io = new SerialInputOutputManager(port, new ReadListener(handle));
            e.io.start();
            sOpen.put(handle, e);
            return 0;
        } catch (Exception ex) {
            Log.e(TAG, "open failed", ex);
            return 3;
        }
    }

    public static int write(long handle, byte[] data) {
        Entry e = sOpen.get(handle);
        if (e == null) return -1;
        try {
            e.port.write(data, 1000);  // 1 s timeout
            return data.length;
        } catch (Exception ex) {
            Log.e(TAG, "write failed", ex);
            return -1;
        }
    }

    public static void close(long handle) {
        Entry e = sOpen.remove(handle);
        if (e == null) return;
        try { if (e.io != null) e.io.stop(); } catch (Exception ignored) {}
        try { if (e.port != null) e.port.close(); } catch (Exception ignored) {}
    }

    // ---- internals -----------------------------------------------------------

    private static final class ReadListener implements SerialInputOutputManager.Listener {
        private final long handle;
        ReadListener(long h) { handle = h; }
        @Override public void onNewData(byte[] data) { nativeOnData(handle, data); }
        @Override public void onRunError(Exception e) {
            Log.e(TAG, "io error", e);
            nativeOnError(handle, 1);
        }
    }

    private static UsbSerialDriver findDriver(UsbManager mgr, String deviceName) {
        if (mgr == null) return null;
        for (UsbSerialDriver d : UsbSerialProber.getDefaultProber().findAllDrivers(mgr)) {
            if (d.getDevice().getDeviceName().equals(deviceName)) return d;
        }
        return null;
    }

    /** Blocks until the user grants/denies USB permission. */
    private static boolean requestPermission(final UsbManager mgr, final Context ctx, final UsbDevice dev) {
        final CountDownLatch latch = new CountDownLatch(1);
        final boolean[] granted = { false };
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent intent) {
                if (ACTION_USB_PERMISSION.equals(intent.getAction())) {
                    granted[0] = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                    latch.countDown();
                }
            }
        };
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) flags |= PendingIntent.FLAG_MUTABLE;
        Intent intent = new Intent(ACTION_USB_PERMISSION);
        intent.setPackage(ctx.getPackageName());
        PendingIntent pi = PendingIntent.getBroadcast(ctx, 0, intent, flags);
        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            ctx.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            ctx.registerReceiver(receiver, filter);
        }
        mgr.requestPermission(dev, pi);
        try { latch.await(30, TimeUnit.SECONDS); } catch (InterruptedException ignored) {}
        try { ctx.unregisterReceiver(receiver); } catch (Exception ignored) {}
        return granted[0];
    }

    // cs enums use Qt values; usb-serial-for-android uses its own constants.
    private static int mapParity(int csParity) {
        // cs/Qt: 0 none, 2 even, 3 odd, 4 space, 5 mark
        // usb :  0 none, 2 even, 1 odd,  4 space, 3 mark
        switch (csParity) {
            case 2: return UsbSerialPort.PARITY_EVEN;
            case 3: return UsbSerialPort.PARITY_ODD;
            case 4: return UsbSerialPort.PARITY_SPACE;
            case 5: return UsbSerialPort.PARITY_MARK;
            default: return UsbSerialPort.PARITY_NONE;
        }
    }

    private static int mapStopBits(int csStop) {
        // cs/Qt and usb-serial both use 1, 2, 3=1.5 - pass through.
        switch (csStop) {
            case 2: return UsbSerialPort.STOPBITS_2;
            case 3: return UsbSerialPort.STOPBITS_1_5;
            default: return UsbSerialPort.STOPBITS_1;
        }
    }

    private static String safeManufacturer(UsbDevice dev) {
        try { return nullToEmpty(dev.getManufacturerName()); }
        catch (SecurityException ignored) { return ""; }
    }

    private static String safeSerial(UsbManager mgr, UsbDevice dev) {
        if (!mgr.hasPermission(dev)) return "";
        try { return nullToEmpty(dev.getSerialNumber()); }
        catch (SecurityException ignored) { return ""; }
    }

    private static String nullToEmpty(String s) { return s == null ? "" : s; }
}
