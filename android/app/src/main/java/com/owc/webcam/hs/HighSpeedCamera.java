package com.owc.webcam.hs;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.hardware.HardwareBuffer;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.MeteringRectangle;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;
import android.util.Range;
import android.util.Size;
import android.view.Surface;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Constrained high-speed Camera2 session (e.g. 120 fps) driven ONLY from native
 * code (libowc_native, camera/high_speed.h + jni_bridge.cpp). The NDK has no
 * createConstrainedHighSpeedCaptureSession, and Camera2 callbacks are abstract
 * classes that JNI cannot implement, hence this small helper.
 *
 * Threading: the public methods are called from the native worker thread
 * (attached to the JVM); camera callbacks run on this object's HandlerThread.
 * The single output is the encoder's input surface. Nothing here is used by
 * the Kotlin UI.
 */
public final class HighSpeedCamera {
    private static final String TAG = "OWC.HS";
    private static volatile String sLastError = "";

    // controls[] layout shared with jni_bridge.cpp (kHsCtl*):
    private static final int CTL_AF_MODE = 0, CTL_AE_LOCK = 1, CTL_AE_COMP = 2, CTL_AWB_MODE = 3,
            CTL_AWB_LOCK = 4, CTL_CROP_X = 5, CTL_CROP_Y = 6, CTL_CROP_W = 7, CTL_CROP_H = 8,
            CTL_TORCH = 9, CTL_COUNT = 10;

    private final HandlerThread thread;
    private final Handler handler;
    private final Surface target;
    private ImageReader dummyPreview;  // self-draining preview stream (HFR use cases want one)
    private final Range<Integer> fpsRange;
    private final boolean realtimeTs;
    private final CountDownLatch closedLatch = new CountDownLatch(1);
    private final AtomicInteger frames = new AtomicInteger();
    private volatile long ispNsEwma;
    private volatile long expNsEwma;
    private volatile String failure;
    private CameraDevice device;
    private CameraConstrainedHighSpeedCaptureSession session;
    private CaptureRequest.Builder builder;

    public static String lastError() {
        return sLastError;
    }

    @SuppressLint({"PrivateApi", "DiscouragedPrivateApi"})
    private static Context appContext() throws Exception {
        Class<?> at = Class.forName("android.app.ActivityThread");
        Object app = at.getMethod("currentApplication").invoke(null);
        if (app == null) throw new IllegalStateException("no Application");
        return (Context) app;
    }

    private static CameraManager cameraManager() throws Exception {
        return (CameraManager) appContext().getSystemService(Context.CAMERA_SERVICE);
    }

    /**
     * "id,w,h,fpsMin,fpsMax,batch;" for every constrained high-speed configuration.
     * batch = batch_size_max from android.control.availableHighSpeedVideoConfigurations
     * (read through a raw int[] key; 0 when unavailable).
     */
    public static String queryConfigs() {
        StringBuilder sb = new StringBuilder();
        try {
            CameraManager cm = cameraManager();
            for (String id : cm.getCameraIdList()) {
                CameraCharacteristics cc = cm.getCameraCharacteristics(id);
                int[] caps = cc.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
                boolean hs = false;
                if (caps != null) {
                    for (int c : caps) {
                        if (c == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO) hs = true;
                    }
                }
                if (!hs) continue;
                StreamConfigurationMap map = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                if (map == null) continue;
                int[] raw = null;
                try {
                    raw = cc.get(new CameraCharacteristics.Key<>(
                            "android.control.availableHighSpeedVideoConfigurations", int[].class));
                } catch (Throwable ignored) {
                }
                for (Size s : map.getHighSpeedVideoSizes()) {
                    for (Range<Integer> r : map.getHighSpeedVideoFpsRangesFor(s)) {
                        int batch = 0;
                        for (int i = 0; raw != null && i + 4 < raw.length; i += 5) {
                            if (raw[i] == s.getWidth() && raw[i + 1] == s.getHeight() && raw[i + 2] == r.getLower()
                                    && raw[i + 3] == r.getUpper()) batch = raw[i + 4];
                        }
                        sb.append(id).append(',').append(s.getWidth()).append(',').append(s.getHeight())
                                .append(',').append(r.getLower()).append(',').append(r.getUpper())
                                .append(',').append(batch).append(';');
                    }
                }
            }
        } catch (Throwable t) {
            sLastError = "queryConfigs: " + t;
            Log.w(TAG, sLastError);
        }
        return sb.toString();
    }

    /** Blocking open; returns null (see {@link #lastError()}) on failure. */
    public static HighSpeedCamera open(String cameraId, Surface target, int width, int height,
                                       int fpsMin, int fpsMax, int[] controls, int previewUsage) {
        HighSpeedCamera c = null;
        try {
            CameraManager cm = cameraManager();
            Integer src = cm.getCameraCharacteristics(cameraId).get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
            c = new HighSpeedCamera(target, fpsMin, fpsMax,
                    src != null && src == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME);
            if (c.start(cm, cameraId, width, height, controls, previewUsage)) return c;
            sLastError = c.failure != null ? c.failure : "unknown failure";
            if (previewUsage != 0) {
                // Default dummy-preview usage (COMPOSER_OVERLAY) refused on this device:
                // retry with the texture usage that is known to work everywhere.
                Log.w(TAG, "preview usage " + previewUsage + " failed (" + sLastError + "); retrying with GPU_SAMPLED_IMAGE");
                c.shutdown(false);
                c = new HighSpeedCamera(target, fpsMin, fpsMax,
                        src != null && src == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME);
                if (c.start(cm, cameraId, width, height, controls, 0)) return c;
                sLastError = c.failure != null ? c.failure : "unknown failure";
            }
        } catch (Throwable t) {
            sLastError = "open: " + t;
        }
        Log.w(TAG, "high-speed open failed: " + sLastError);
        if (c != null) c.close();
        return null;
    }

    private HighSpeedCamera(Surface target, int fpsMin, int fpsMax, boolean realtimeTs) {
        this.target = target;
        this.fpsRange = new Range<>(fpsMin, fpsMax);
        this.realtimeTs = realtimeTs;
        thread = new HandlerThread("owc-hs-camera");
        thread.start();
        handler = new Handler(thread.getLooper());
    }

    private void fail(String why) {
        if (failure == null) failure = why;
        Log.w(TAG, why);
    }

    @SuppressLint("MissingPermission")
    @SuppressWarnings("deprecation")  // createConstrainedHighSpeedCaptureSession: fine on API 26+
    private static long usageFor(int mode) {
        switch (mode) {
            case 1: return HardwareBuffer.USAGE_VIDEO_ENCODE;
            case 2: return HardwareBuffer.USAGE_COMPOSER_OVERLAY;
            case 3: return HardwareBuffer.USAGE_GPU_COLOR_OUTPUT;
            default: return HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE;
        }
    }

    private boolean start(CameraManager cm, String id, int width, int height, int[] controls,
                          int previewUsage) throws Exception {
        final CountDownLatch opened = new CountDownLatch(1);
        cm.openCamera(id, new CameraDevice.StateCallback() {
            @Override public void onOpened(CameraDevice d) {
                device = d;
                opened.countDown();
            }
            @Override public void onDisconnected(CameraDevice d) {
                fail("camera disconnected");
                d.close();
                opened.countDown();
            }
            @Override public void onError(CameraDevice d, int error) {
                fail("camera error " + error);
                d.close();
                opened.countDown();
            }
            @Override public void onClosed(CameraDevice d) {
                closedLatch.countDown();
            }
        }, handler);
        if (!opened.await(3, TimeUnit.SECONDS)) fail("camera open timeout");
        if (device == null || failure != null) return false;

        // Qualcomm/Samsung HFR pipelines expect a preview + a video stream; a
        // video-only session gets buffer errors. The preview is an ImageReader
        // that is drained immediately (only 1 request per burst targets it).
        List<Surface> outputs = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= 29) {
            dummyPreview = ImageReader.newInstance(width, height, ImageFormat.PRIVATE, 3,
                    usageFor(previewUsage));
            Log.i(TAG, "dummy preview usage mode " + previewUsage + " = 0x" + Long.toHexString(usageFor(previewUsage)));
            dummyPreview.setOnImageAvailableListener(r -> {
                Image img = r.acquireLatestImage();
                if (img != null) img.close();
            }, handler);
            outputs.add(dummyPreview.getSurface());
        }
        outputs.add(target);
        final CountDownLatch configured = new CountDownLatch(1);
        device.createConstrainedHighSpeedCaptureSession(outputs,
                new CameraCaptureSession.StateCallback() {
                    @Override public void onConfigured(CameraCaptureSession s) {
                        session = (CameraConstrainedHighSpeedCaptureSession) s;
                        configured.countDown();
                    }
                    @Override public void onConfigureFailed(CameraCaptureSession s) {
                        fail("high-speed session configuration failed");
                        configured.countDown();
                    }
                }, handler);
        if (!configured.await(3, TimeUnit.SECONDS)) fail("session configure timeout");
        if (session == null || failure != null) return false;

        builder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
        if (dummyPreview != null) builder.addTarget(dummyPreview.getSurface());
        builder.addTarget(target);
        builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange);
        setControls(controls);
        submitRepeating();
        Log.i(TAG, "high-speed session running at " + fpsRange);
        return failure == null;
    }

    private final CameraCaptureSession.CaptureCallback captureCb = new CameraCaptureSession.CaptureCallback() {
        private long lastTs, diagSumNs;
        private int diagCount;

        @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult result) {
            Long ts = result.get(CaptureResult.SENSOR_TIMESTAMP);
            if (ts == null) {
                frames.incrementAndGet();
                return;
            }
            // Results can arrive out of order / more than once per sensor frame
            // (seen on SM-N9600 HFR): count only timestamps that advance.
            if (ts <= lastTs) return;
            if (lastTs != 0 && diagCount < 1000) {
                diagSumNs += ts - lastTs;
                if (++diagCount == 1000) {
                    Log.i(TAG, "sensor frame spacing avg " + (diagSumNs / 1000 / 1000.0) + " us over 1000 frames");
                }
            }
            lastTs = ts;
            frames.incrementAndGet();
            long now = realtimeTs ? SystemClock.elapsedRealtimeNanos() : System.nanoTime();
            long d = now - ts;
            if (d <= 0 || d > 2_000_000_000L) return;
            long prev = ispNsEwma;
            ispNsEwma = prev == 0 ? d : prev + (d - prev) / 16;
            Long exp = result.get(CaptureResult.SENSOR_EXPOSURE_TIME);
            if (exp != null) {
                long pe = expNsEwma;
                expNsEwma = pe == 0 ? exp : pe + (exp - pe) / 16;
            }
        }
    };

    private void setControls(int[] c) {
        if (c == null || c.length < CTL_COUNT) return;
        builder.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
        builder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
        builder.set(CaptureRequest.CONTROL_AF_MODE, c[CTL_AF_MODE]);
        builder.set(CaptureRequest.CONTROL_AE_LOCK, c[CTL_AE_LOCK] != 0);
        builder.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, c[CTL_AE_COMP]);
        builder.set(CaptureRequest.CONTROL_AWB_MODE, c[CTL_AWB_MODE]);
        builder.set(CaptureRequest.CONTROL_AWB_LOCK, c[CTL_AWB_LOCK] != 0);
        if (c[CTL_CROP_W] > 0 && c[CTL_CROP_H] > 0) {
            builder.set(CaptureRequest.SCALER_CROP_REGION, new Rect(c[CTL_CROP_X], c[CTL_CROP_Y],
                    c[CTL_CROP_X] + c[CTL_CROP_W], c[CTL_CROP_Y] + c[CTL_CROP_H]));
        }
        builder.set(CaptureRequest.FLASH_MODE,
                c[CTL_TORCH] != 0 ? CaptureRequest.FLASH_MODE_TORCH : CaptureRequest.FLASH_MODE_OFF);
        builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE);
    }

    private void submitRepeating() throws CameraAccessException {
        List<CaptureRequest> burst = session.createHighSpeedRequestList(builder.build());
        session.setRepeatingBurst(burst, captureCb, handler);
    }

    /** Re-applies the controls on the repeating burst. */
    public synchronized boolean apply(int[] controls) {
        try {
            if (session == null) return false;
            setControls(controls);
            submitRepeating();
            return true;
        } catch (Throwable t) {
            fail("apply: " + t);
            return false;
        }
    }

    /** One-shot AF at the region (active-array coordinates), then back to repeating. */
    public synchronized boolean focusAt(int x0, int y0, int x1, int y1) {
        try {
            if (session == null) return false;
            MeteringRectangle[] region = {new MeteringRectangle(x0, y0, Math.max(1, x1 - x0), Math.max(1, y1 - y0), 1000)};
            builder.set(CaptureRequest.CONTROL_AF_REGIONS, region);
            builder.set(CaptureRequest.CONTROL_AE_REGIONS, region);
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_CANCEL);
            session.captureBurst(session.createHighSpeedRequestList(builder.build()), null, handler);
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START);
            session.captureBurst(session.createHighSpeedRequestList(builder.build()), null, handler);
            builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE);
            submitRepeating();
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "focusAt: " + t);
            return false;
        }
    }

    public int takeFrames() {
        return frames.getAndSet(0);
    }

    public float ispLatencyMs() {
        return ispNsEwma / 1e6f;
    }

    public float exposureMs() {
        return expNsEwma / 1e6f;
    }

    /** Null while healthy. */
    public String failure() {
        return failure;
    }

    /** Blocking close (<= ~2 s). Idempotent. Releases the target surface reference. */
    public synchronized void close() {
        shutdown(true);
    }

    private synchronized void shutdown(boolean releaseTarget) {
        try {
            if (session != null) {
                try {
                    session.stopRepeating();
                } catch (Throwable ignored) {
                }
                session.close();
                session = null;
            }
            if (device != null) {
                device.close();
                device = null;
                closedLatch.await(2, TimeUnit.SECONDS);
            }
        } catch (Throwable t) {
            Log.w(TAG, "close: " + t);
        }
        if (dummyPreview != null) {
            dummyPreview.close();
            dummyPreview = null;
        }
        thread.quitSafely();
        try {
            thread.join(1000);
        } catch (InterruptedException ignored) {
        }
        if (releaseTarget) target.release();  // drops the reference taken by ANativeWindow_toSurface
    }
}
