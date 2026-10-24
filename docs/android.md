# CrossSerial on Android — build & integration

On Android, CrossSerial is backed by the vendored **usb-serial-for-android**
library (FTDI, CP21xx, CH34x, PL2303, CDC/ACM, ... drivers). It is compiled to
a `classes.dex` and **embedded into `libcrossserial.so`** (loaded at runtime via
a `DexClassLoader`). The consumer never compiles or links Java - only the `.so`
is bundled into the app.

## 1. Prerequisites

| Tool | Needed for | Notes |
|------|------------|-------|
| Android NDK | compiling the `.so` | set `ANDROID_NDK_ROOT` (or `ANDROID_NDK`) |
| Android SDK | `android.jar` + `d8` (dex) | set `ANDROID_SDK_ROOT`; install a **platform** (`platforms/android-34`+) and **build-tools** |
| JDK (javac) | compiling java → dex | JDK 11+; `javac` on PATH or `JAVA_HOME` set |
| CMake 3.16+, Ninja/Make | the build | |

## 2. Build all ABIs

```sh
scripts/build-android.sh                 # arm64-v8a armeabi-v7a x86 x86_64
scripts/build-android.sh arm64-v8a       # subset
MIN_API=android-21 scripts/build-android.sh
OUT_DIR=dist/android scripts/build-android.sh
```

Output layout (`jniLibs/<abi>/libcrossserial.so`, each with the dex embedded):

```
dist/android/
  jniLibs/
    arm64-v8a/libcrossserial.so
    armeabi-v7a/libcrossserial.so
    ...
  include/cs/
```

A single-ABI manual build works too:

```sh
cmake -S . -B build-android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DANDROID_SDK_ROOT=$ANDROID_SDK_ROOT \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android-arm64
```

## 3. Consume in an Android app

### 3a. Bundle the native libraries

Copy the `jniLibs/<abi>/` folders into the Gradle app module:

```
app/src/main/jniLibs/
  arm64-v8a/libcrossserial.so
  ...
```

### 3b. Load + initialize once

In Kotlin (Java is analogous):

```kotlin
class MyApp : Application() {
    override fun onCreate() {
        super.onCreate()
        System.loadLibrary("crossserial")   // triggers JNI_OnLoad (captures the JavaVM)
        initAndroid(this)                   // pass the Context
    }

    private external fun initAndroid(ctx: Any)  // -> cs::initAndroid(nullptr, ctx)
}
```

The `Context` is required for `UsbManager` access and to extract the embedded
dex into the code cache. `cs::initAndroid(nullptr, context)` is the C++ entry
point (`cs::SerialPort.hpp`, Android-only declaration).

### 3c. Use the normal C++ API

```cpp
cs::SerialPort port;
port.setPortName(deviceName);          // from cs::SerialPortInfo::availablePorts()
port.setBaudRate(115200);
port.registerOnReadyRead([&] { /* port.readAll() ... */ });
port.open();
```

### 3d. USB permission

By default `open()` requests USB permission itself: the system dialog is shown
and `open()` blocks until the user answers (max 30 s); a denial fails the open
with `cs::SerialError::PermissionError`.

Apps that prefer to drive permissions from Kotlin/Java (e.g. react to the
`UsbManager.ACTION_USB_PERMISSION` broadcast themselves, or use the
device-filter `<meta-data>` auto-grant flow) can disable the auto-request:

```cpp
port.setPermissionRequestEnabled(false);   // open() then fails fast with
                                           // PermissionError until the user
                                           // has granted access
```

The flag is per-port, and a no-op on desktop platforms.

## 4. Notes

- **Threading:** `onReadyRead` / `onError` fire on the Java I/O-manager thread;
  marshal to the UI thread if needed.
- **Port names** are USB device names (`/dev/bus/usb/001/002`) - take them from
  `cs::SerialPortInfo::availablePorts()`, never hardcode.
- **Config:** `setBaudRate/setDataBits/setParity/setStopBits` are applied at
  `open()` (the driver API has no live reconfigure); changing them on an open
  port only takes effect after close/open. Flow control is not supported by the
  driver and is ignored.
- `serialNumber` is only readable after USB permission has been granted.
- Ship only the ABIs you need (usually `arm64-v8a` + `armeabi-v7a`).
