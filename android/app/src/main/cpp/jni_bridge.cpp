// JNI contract (docs/ARCHITECTURE.md "Contrato JNI"):
//   class com.owc.webcam.NativeBridge (Kotlin object, @JvmStatic) — static natives:
//     nativeStart(String,String,int,int,int,int)Z, nativeStop()V,
//     nativeSetPreviewSurface(Surface)V, nativeGetStatus()String,
//     nativeUpdateDeviceStatus(int,float,boolean,int)V, nativeSetDeviceName(String)V, nativeScreenModeChanged(int)V,
//     nativeSetDataDir(String)Z (before nativeStart), nativeGetPairedClients()String,
//     nativeUnpairClient(String)Z, nativeCancelPairing()V — the pairing calls work
//     whether or not the core is running.
//   callback: static onNativeEvent(int,int,String)V, invoked from a dedicated
//   native thread ("owc-jni-ev", attached once) so no camera/encoder/network
//   thread ever runs Java code or waits for it.
#include <android/native_window_jni.h>
#include <camera/NdkCameraMetadataTags.h>
#include <jni.h>

#include <cctype>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "app_core.h"
#include "camera/high_speed.h"
#include "util/log.h"
#include "util/thread.h"

using owc::AppCore;

namespace {

JavaVM* g_vm = nullptr;
jclass g_bridgeClass = nullptr;  // GlobalRef
jmethodID g_onNativeEvent = nullptr;

// com.owc.webcam.hs.HighSpeedCamera (optional helper; cached in JNI_OnLoad
// because native threads cannot FindClass app classes).
struct HsJava {
  jclass cls = nullptr;  // GlobalRef
  jmethodID queryConfigs = nullptr, open = nullptr, lastError = nullptr, apply = nullptr,
            focusAt = nullptr, takeFrames = nullptr, ispLatencyMs = nullptr, exposureMs = nullptr,
            failure = nullptr,
            close = nullptr;
} g_hs;

bool clearExc(JNIEnv* env);

void cacheHighSpeedClass(JNIEnv* env) {
  jclass c = env->FindClass("com/owc/webcam/hs/HighSpeedCamera");
  if (!c || env->ExceptionCheck()) {
    env->ExceptionClear();
    LOGW_LOCAL("HighSpeedCamera helper class missing: high-speed mode disabled");
    return;
  }
  HsJava h;
  h.queryConfigs = env->GetStaticMethodID(c, "queryConfigs", "()Ljava/lang/String;");
  h.open = env->GetStaticMethodID(c, "open",
                                  "(Ljava/lang/String;Landroid/view/Surface;IIII[II)Lcom/owc/webcam/hs/HighSpeedCamera;");
  h.lastError = env->GetStaticMethodID(c, "lastError", "()Ljava/lang/String;");
  h.apply = env->GetMethodID(c, "apply", "([I)Z");
  h.focusAt = env->GetMethodID(c, "focusAt", "(IIII)Z");
  h.takeFrames = env->GetMethodID(c, "takeFrames", "()I");
  h.ispLatencyMs = env->GetMethodID(c, "ispLatencyMs", "()F");
  h.exposureMs = env->GetMethodID(c, "exposureMs", "()F");
  h.failure = env->GetMethodID(c, "failure", "()Ljava/lang/String;");
  h.close = env->GetMethodID(c, "close", "()V");
  if (clearExc(env) || !h.queryConfigs || !h.open || !h.lastError || !h.apply || !h.focusAt ||
      !h.takeFrames || !h.ispLatencyMs || !h.exposureMs || !h.failure || !h.close) {
    LOGW_LOCAL("HighSpeedCamera helper has an unexpected shape: high-speed mode disabled");
    env->DeleteLocalRef(c);
    return;
  }
  h.cls = static_cast<jclass>(env->NewGlobalRef(c));
  env->DeleteLocalRef(c);
  g_hs = h;
}

// The worker thread is attached on first use and detached by detachThread().
thread_local bool t_attachedByHs = false;

JNIEnv* hsEnv() {
  JNIEnv* env = nullptr;
  if (!g_vm) return nullptr;
  jint r = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (r == JNI_OK) return env;
  if (r != JNI_EDETACHED) return nullptr;
  JavaVMAttachArgs args{JNI_VERSION_1_6, "owc-worker", nullptr};
  if (g_vm->AttachCurrentThread(&env, &args) != JNI_OK) return nullptr;
  t_attachedByHs = true;
  return env;
}

std::string javaString(JNIEnv* env, jstring s);

// Layout of the int[] controls shared with HighSpeedCamera.java.
jintArray hsControls(JNIEnv* env, const owc::CameraDesc& d, const owc::CameraControls& c) {
  int32_t crop[4] = {0, 0, 0, 0};
  if (d.activeW > 0 && d.activeH > 0) owc::cropForZoom(d, c.zoom, crop);
  int af = owc::cameraSupportsAfMode(d, c.afMode) && c.afMode != 0 ? owc::afModeToCamera(c.afMode)
                                                                   : ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
  if (d.afModes.empty()) af = ACAMERA_CONTROL_AF_MODE_OFF;
  const jint v[10] = {af,
                      c.aeLock && d.aeLockAvailable ? 1 : 0,
                      std::clamp(c.aeCompensation, d.aeCompMin, d.aeCompMax),
                      d.hasMode(d.awbModes, static_cast<uint8_t>(c.awbMode)) ? c.awbMode
                                                                              : ACAMERA_CONTROL_AWB_MODE_AUTO,
                      c.awbLock && d.awbLockAvailable ? 1 : 0,
                      crop[0], crop[1], crop[2], crop[3],
                      c.torch && d.info.has_flash ? 1 : 0};
  jintArray a = env->NewIntArray(10);
  if (a) env->SetIntArrayRegion(a, 0, 10, v);
  return a;
}

class JniHighSpeed : public owc::HighSpeedBackend {
 public:
  bool open(const owc::CameraDesc& d, ANativeWindow* target, int width, int height, int fpsMin,
            int fpsMax, const owc::CameraControls& c, int previewUsage, std::string& err) override {
    close();
    JNIEnv* env = hsEnv();
    if (!env || !g_hs.cls) {
      err = "JNI unavailable";
      return false;
    }
    jobject surface = ANativeWindow_toSurface(env, target);
    jstring id = env->NewStringUTF(d.info.id.c_str());
    jintArray ctl = hsControls(env, d, c);
    jobject o = surface ? env->CallStaticObjectMethod(g_hs.cls, g_hs.open, id, surface, width, height, fpsMin, fpsMax, ctl,
                                             previewUsage)
                        : nullptr;
    if (clearExc(env)) o = nullptr;
    if (o) {
      obj_ = env->NewGlobalRef(o);
      env->DeleteLocalRef(o);
    } else {
      jstring e = static_cast<jstring>(env->CallStaticObjectMethod(g_hs.cls, g_hs.lastError));
      clearExc(env);
      err = surface ? javaString(env, e) : "ANativeWindow_toSurface failed";
      if (e) env->DeleteLocalRef(e);
    }
    if (ctl) env->DeleteLocalRef(ctl);
    env->DeleteLocalRef(id);
    if (surface) env->DeleteLocalRef(surface);
    return obj_ != nullptr;
  }

  void close() override {
    if (!obj_) return;
    JNIEnv* env = hsEnv();
    if (!env) return;
    env->CallVoidMethod(obj_, g_hs.close);
    clearExc(env);
    env->DeleteGlobalRef(obj_);
    obj_ = nullptr;
  }

  bool apply(const owc::CameraDesc& d, const owc::CameraControls& c) override {
    JNIEnv* env = obj_ ? hsEnv() : nullptr;
    if (!env) return false;
    jintArray ctl = hsControls(env, d, c);
    jboolean ok = env->CallBooleanMethod(obj_, g_hs.apply, ctl);
    if (clearExc(env)) ok = JNI_FALSE;
    if (ctl) env->DeleteLocalRef(ctl);
    return ok == JNI_TRUE;
  }

  bool focusAt(const int32_t r[4]) override {
    JNIEnv* env = obj_ ? hsEnv() : nullptr;
    if (!env) return false;
    jboolean ok = env->CallBooleanMethod(obj_, g_hs.focusAt, r[0], r[1], r[2], r[3]);
    if (clearExc(env)) ok = JNI_FALSE;
    return ok == JNI_TRUE;
  }

  uint32_t takeFrames() override {
    JNIEnv* env = obj_ ? hsEnv() : nullptr;
    if (!env) return 0;
    jint n = env->CallIntMethod(obj_, g_hs.takeFrames);
    return clearExc(env) || n < 0 ? 0u : static_cast<uint32_t>(n);
  }

  float ispLatencyMs() override {
    JNIEnv* env = obj_ ? hsEnv() : nullptr;
    if (!env) return 0;
    jfloat v = env->CallFloatMethod(obj_, g_hs.ispLatencyMs);
    return clearExc(env) ? 0.0f : v;
  }

  float exposureMs() override {
    JNIEnv* env = obj_ ? hsEnv() : nullptr;
    if (!env) return 0;
    jfloat v = env->CallFloatMethod(obj_, g_hs.exposureMs);
    return clearExc(env) ? 0.0f : v;
  }

  bool failed(std::string& why) override {
    JNIEnv* env = obj_ ? hsEnv() : nullptr;
    if (!env) return false;
    jstring s = static_cast<jstring>(env->CallObjectMethod(obj_, g_hs.failure));
    if (clearExc(env) || !s) return false;
    why = javaString(env, s);
    env->DeleteLocalRef(s);
    return true;
  }

  void detachThread() override {
    if (t_attachedByHs && g_vm) {
      g_vm->DetachCurrentThread();
      t_attachedByHs = false;
    }
  }

 private:
  jobject obj_ = nullptr;  // GlobalRef to the HighSpeedCamera instance (worker thread only)
};

JniHighSpeed g_hsBackend;

// ------------------------------------------------------------------ events
class JniEventDispatcher : public owc::EventSink {
 public:
  void start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread(&JniEventDispatcher::loop, this);
  }
  // Delivers what is queued, then joins. Safe to call from the dispatcher
  // thread itself (Java calling nativeStop from onNativeEvent): no self-join.
  void stop() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      if (thread_.get_id() == std::this_thread::get_id()) {
        thread_.detach();  // exits right after the current callback returns
        LOGW_LOCAL("nativeStop called from onNativeEvent: event thread detached");
      } else {
        thread_.join();
      }
    }
  }
  void onEvent(int type, int i, const std::string& s) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (q_.size() > 256) q_.pop_front();
      q_.push_back({type, i, s});
    }
    cv_.notify_one();
  }

 private:
  struct Ev {
    int type, i;
    std::string s;
  };
  void loop() {
    owc::setThreadName("owc-jni-ev");
    JNIEnv* env = nullptr;
    JavaVMAttachArgs args{JNI_VERSION_1_6, "owc-jni-ev", nullptr};
    if (!g_vm || g_vm->AttachCurrentThread(&env, &args) != JNI_OK) {
      LOGE_LOCAL("event thread: AttachCurrentThread failed");
      return;
    }
    for (;;) {
      Ev ev;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
        if (q_.empty()) break;  // stop_ and drained
        ev = std::move(q_.front());
        q_.pop_front();
      }
      if (!g_bridgeClass || !g_onNativeEvent) continue;
      jstring js = ev.s.empty() ? nullptr : env->NewStringUTF(ev.s.c_str());
      env->CallStaticVoidMethod(g_bridgeClass, g_onNativeEvent, static_cast<jint>(ev.type),
                                static_cast<jint>(ev.i), js);
      if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
      }
      if (js) env->DeleteLocalRef(js);
    }
    g_vm->DetachCurrentThread();
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Ev> q_;
  bool stop_ = false;
  std::thread thread_;
};

std::mutex g_mu;  // guards g_core / g_events lifecycle
std::unique_ptr<AppCore> g_core;
std::unique_ptr<JniEventDispatcher> g_events;

AppCore& core() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_core) g_core = std::make_unique<AppCore>();
  return *g_core;
}

std::string toStd(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

std::string javaString(JNIEnv* env, jstring s) { return toStd(env, s); }

// "id,w,h,min,max;..." from HighSpeedCamera.queryConfigs().
void queryHighSpeed(JNIEnv* env, owc::StartOptions& o) {
  if (!g_hs.cls) return;
  jstring s = static_cast<jstring>(env->CallStaticObjectMethod(g_hs.cls, g_hs.queryConfigs));
  if (clearExc(env) || !s) return;
  const std::string all = toStd(env, s);
  env->DeleteLocalRef(s);
  size_t pos = 0;
  while (pos < all.size()) {
    size_t end = all.find(';', pos);
    if (end == std::string::npos) end = all.size();
    const std::string item = all.substr(pos, end - pos);
    pos = end + 1;
    char id[64] = {0};
    owc::HsConfig c;
    if (sscanf(item.c_str(), "%63[^,],%d,%d,%d,%d,%d", id, &c.width, &c.height, &c.fpsMin, &c.fpsMax,
               &c.batch) >= 5)
      o.hsConfigs.emplace_back(id, c);
  }
  o.hsBackend = &g_hsBackend;
  LOGI("high-speed configs: %s", all.empty() ? "(none)" : all.c_str());
}

// ------------------------------------------------------------------ MediaCodecList
// Optional: richer encoder capabilities than the NDK offers (profiles,
// bitrate range, intra-refresh, size limits, bitrate modes). Every failure
// just leaves javaInfo=false.
bool clearExc(JNIEnv* env) {
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return true;
  }
  return false;
}

struct LocalRef {
  JNIEnv* env;
  jobject obj;
  LocalRef(JNIEnv* e, jobject o) : env(e), obj(o) {}
  ~LocalRef() {
    if (obj) env->DeleteLocalRef(obj);
  }
  LocalRef(const LocalRef&) = delete;
  LocalRef& operator=(const LocalRef&) = delete;
};

int rangeUpper(JNIEnv* env, jobject range) {
  if (!range) return 0;
  LocalRef rc(env, env->GetObjectClass(range));
  jmethodID getUpper = env->GetMethodID(static_cast<jclass>(rc.obj), "getUpper", "()Ljava/lang/Comparable;");
  if (clearExc(env) || !getUpper) return 0;
  LocalRef up(env, env->CallObjectMethod(range, getUpper));
  if (clearExc(env) || !up.obj) return 0;
  LocalRef ic(env, env->FindClass("java/lang/Integer"));
  if (clearExc(env) || !ic.obj) return 0;
  jmethodID intValue = env->GetMethodID(static_cast<jclass>(ic.obj), "intValue", "()I");
  if (clearExc(env) || !intValue) return 0;
  int v = env->CallIntMethod(up.obj, intValue);
  return clearExc(env) ? 0 : v;
}

void queryJavaCodecs(JNIEnv* env, int api, owc::EncoderCapsSet& out) {
  if (env->PushLocalFrame(256) != 0) {
    clearExc(env);
    return;
  }
  do {
    jclass listCls = env->FindClass("android/media/MediaCodecList");
    if (clearExc(env) || !listCls) break;
    jmethodID ctor = env->GetMethodID(listCls, "<init>", "(I)V");
    jmethodID getInfos = env->GetMethodID(listCls, "getCodecInfos", "()[Landroid/media/MediaCodecInfo;");
    if (clearExc(env) || !ctor || !getInfos) break;
    jobject list = env->NewObject(listCls, ctor, 0 /* REGULAR_CODECS */);
    if (clearExc(env) || !list) break;
    auto infos = static_cast<jobjectArray>(env->CallObjectMethod(list, getInfos));
    if (clearExc(env) || !infos) break;

    jclass infoCls = env->FindClass("android/media/MediaCodecInfo");
    jclass capsCls = env->FindClass("android/media/MediaCodecInfo$CodecCapabilities");
    jclass plCls = env->FindClass("android/media/MediaCodecInfo$CodecProfileLevel");
    jclass vcCls = env->FindClass("android/media/MediaCodecInfo$VideoCapabilities");
    jclass ecCls = env->FindClass("android/media/MediaCodecInfo$EncoderCapabilities");
    if (clearExc(env) || !infoCls || !capsCls || !plCls || !vcCls || !ecCls) break;
    jmethodID isEncoder = env->GetMethodID(infoCls, "isEncoder", "()Z");
    jmethodID getName = env->GetMethodID(infoCls, "getName", "()Ljava/lang/String;");
    jmethodID getTypes = env->GetMethodID(infoCls, "getSupportedTypes", "()[Ljava/lang/String;");
    jmethodID getCaps = env->GetMethodID(infoCls, "getCapabilitiesForType",
                                         "(Ljava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;");
    if (clearExc(env) || !isEncoder || !getName || !getTypes || !getCaps) break;
    jmethodID isHw = api >= 29 ? env->GetMethodID(infoCls, "isHardwareAccelerated", "()Z") : nullptr;
    clearExc(env);
    jfieldID plField = env->GetFieldID(capsCls, "profileLevels",
                                       "[Landroid/media/MediaCodecInfo$CodecProfileLevel;");
    jfieldID plProfile = env->GetFieldID(plCls, "profile", "I");
    jfieldID plLevel = env->GetFieldID(plCls, "level", "I");
    jmethodID isFeature = env->GetMethodID(capsCls, "isFeatureSupported", "(Ljava/lang/String;)Z");
    jmethodID getVideoCaps = env->GetMethodID(capsCls, "getVideoCapabilities",
                                              "()Landroid/media/MediaCodecInfo$VideoCapabilities;");
    jmethodID getEncCaps = env->GetMethodID(capsCls, "getEncoderCapabilities",
                                            "()Landroid/media/MediaCodecInfo$EncoderCapabilities;");
    jmethodID getBitrate = env->GetMethodID(vcCls, "getBitrateRange", "()Landroid/util/Range;");
    jmethodID getWidths = env->GetMethodID(vcCls, "getSupportedWidths", "()Landroid/util/Range;");
    jmethodID getHeights = env->GetMethodID(vcCls, "getSupportedHeights", "()Landroid/util/Range;");
    jmethodID isModeOk = env->GetMethodID(ecCls, "isBitrateModeSupported", "(I)Z");
    if (clearExc(env) || !plField || !plProfile || !plLevel || !isFeature || !getVideoCaps ||
        !getEncCaps || !getBitrate || !getWidths || !getHeights || !isModeOk)
      break;
    jstring featIntra = env->NewStringUTF("intra-refresh");

    const jsize n = env->GetArrayLength(infos);
    for (jsize i = 0; i < n; ++i) {
      LocalRef info(env, env->GetObjectArrayElement(infos, i));
      if (!info.obj || !env->CallBooleanMethod(info.obj, isEncoder) || clearExc(env)) continue;
      LocalRef jname(env, env->CallObjectMethod(info.obj, getName));
      if (clearExc(env)) continue;
      const std::string name = toStd(env, static_cast<jstring>(jname.obj));
      bool hw = name.rfind("OMX.google.", 0) != 0 && name.rfind("c2.android.", 0) != 0;
      if (isHw) {
        hw = env->CallBooleanMethod(info.obj, isHw);
        if (clearExc(env)) hw = false;
      }
      LocalRef types(env, env->CallObjectMethod(info.obj, getTypes));
      if (clearExc(env) || !types.obj) continue;
      const jsize nt = env->GetArrayLength(static_cast<jobjectArray>(types.obj));
      for (jsize t = 0; t < nt; ++t) {
        LocalRef jt(env, env->GetObjectArrayElement(static_cast<jobjectArray>(types.obj), t));
        std::string type = toStd(env, static_cast<jstring>(jt.obj));
        for (auto& ch : type) ch = static_cast<char>(tolower(ch));
        owc::EncoderCaps* target = type == "video/avc" ? &out.avc : type == "video/hevc" ? &out.hevc : nullptr;
        if (!target) continue;
        // Keep the first hardware encoder (list order = platform preference);
        // a software one only if nothing better was seen.
        if (target->javaInfo && (target->hardware || !hw)) continue;
        LocalRef caps(env, env->CallObjectMethod(info.obj, getCaps, jt.obj));
        if (clearExc(env) || !caps.obj) continue;
        owc::EncoderCaps c;
        c.name = name;
        c.mime = type;
        c.hardware = hw;
        c.javaInfo = true;
        c.available = true;
        LocalRef pls(env, env->GetObjectField(caps.obj, plField));
        if (pls.obj) {
          const jsize np = env->GetArrayLength(static_cast<jobjectArray>(pls.obj));
          for (jsize k = 0; k < np; ++k) {
            LocalRef pl(env, env->GetObjectArrayElement(static_cast<jobjectArray>(pls.obj), k));
            if (!pl.obj) continue;
            c.profileLevels.emplace_back(env->GetIntField(pl.obj, plProfile), env->GetIntField(pl.obj, plLevel));
          }
        }
        c.intraRefresh = env->CallBooleanMethod(caps.obj, isFeature, featIntra);
        if (clearExc(env)) c.intraRefresh = false;
        LocalRef vc(env, env->CallObjectMethod(caps.obj, getVideoCaps));
        if (!clearExc(env) && vc.obj) {
          LocalRef br(env, env->CallObjectMethod(vc.obj, getBitrate));
          if (!clearExc(env)) c.maxBitrate = rangeUpper(env, br.obj);
          LocalRef w(env, env->CallObjectMethod(vc.obj, getWidths));
          if (!clearExc(env)) c.maxWidth = rangeUpper(env, w.obj);
          LocalRef h(env, env->CallObjectMethod(vc.obj, getHeights));
          if (!clearExc(env)) c.maxHeight = rangeUpper(env, h.obj);
        }
        LocalRef ec(env, env->CallObjectMethod(caps.obj, getEncCaps));
        if (!clearExc(env) && ec.obj) {
          for (int mode = 0; mode <= 2; ++mode) {
            bool ok = env->CallBooleanMethod(ec.obj, isModeOk, mode);
            if (!clearExc(env) && ok) c.bitrateModes |= 1u << mode;
          }
        }
        *target = std::move(c);
        LOGI("MediaCodecList: %s %s hw=%d profiles=%zu intraRefresh=%d maxBitrate=%d max=%dx%d modes=0x%x",
             target->mime.c_str(), target->name.c_str(), target->hardware, target->profileLevels.size(),
             target->intraRefresh, target->maxBitrate, target->maxWidth, target->maxHeight,
             target->bitrateModes);
      }
    }
  } while (false);
  clearExc(env);
  env->PopLocalFrame(nullptr);
}

}  // namespace

// ------------------------------------------------------------------ exports
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  g_vm = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass cls = env->FindClass("com/owc/webcam/NativeBridge");
  if (!cls || env->ExceptionCheck()) {
    env->ExceptionClear();
    LOGE_LOCAL("JNI_OnLoad: class com.owc.webcam.NativeBridge not found; events disabled");
    return JNI_VERSION_1_6;
  }
  g_bridgeClass = static_cast<jclass>(env->NewGlobalRef(cls));
  env->DeleteLocalRef(cls);
  g_onNativeEvent = env->GetStaticMethodID(g_bridgeClass, "onNativeEvent", "(IILjava/lang/String;)V");
  if (!g_onNativeEvent || env->ExceptionCheck()) {
    env->ExceptionClear();
    g_onNativeEvent = nullptr;
    LOGE_LOCAL("JNI_OnLoad: onNativeEvent(IILjava/lang/String;)V not found; events disabled");
  }
  cacheHighSpeedClass(env);
  return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_owc_webcam_NativeBridge_nativeStart(
    JNIEnv* env, jclass, jstring deviceName, jstring model, jint apiLevel, jint controlPort,
    jint udpPort, jint discoveryPort) {
  owc::StartOptions o;
  o.deviceName = toStd(env, deviceName);
  o.model = toStd(env, model);
  o.apiLevel = apiLevel;
  o.controlPort = static_cast<uint16_t>(controlPort);
  o.udpPort = static_cast<uint16_t>(udpPort);
  o.discoveryPort = static_cast<uint16_t>(discoveryPort);
  queryJavaCodecs(env, apiLevel, o.javaCaps);  // ~10-50 ms, on the calling (Java) thread
  queryHighSpeed(env, o);
  o.javaCaps.apiLevel = apiLevel;
  AppCore& c = core();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_events) g_events = std::make_unique<JniEventDispatcher>();
    g_events->start();
  }
  std::string err;
  bool ok = c.start(o, g_events.get(), err);
  if (!ok) {
    LOGE_LOCAL("nativeStart failed: %s", err.c_str());
    g_events->onEvent(owc::kEvError, 0, err);
  }
  return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_owc_webcam_NativeBridge_nativeStop(JNIEnv*, jclass) {
  core().stop();
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_events) g_events->stop();  // delivers pending events, then joins
}

extern "C" JNIEXPORT void JNICALL Java_com_owc_webcam_NativeBridge_nativeSetPreviewSurface(
    JNIEnv* env, jclass, jobject surface) {
  ANativeWindow* w = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;  // +1 ref
  core().setPreviewSurface(w);  // takes the reference
}

extern "C" JNIEXPORT jstring JNICALL Java_com_owc_webcam_NativeBridge_nativeGetStatus(JNIEnv* env,
                                                                                       jclass) {
  return env->NewStringUTF(core().statusJson().c_str());
}

extern "C" JNIEXPORT void JNICALL Java_com_owc_webcam_NativeBridge_nativeUpdateDeviceStatus(
    JNIEnv*, jclass, jint batteryPct, jfloat batteryTempC, jboolean charging, jint thermalStatus) {
  core().updateDeviceStatus(batteryPct, batteryTempC, charging == JNI_TRUE, thermalStatus);
}

extern "C" JNIEXPORT void JNICALL Java_com_owc_webcam_NativeBridge_nativeSetDeviceName(JNIEnv* env,
                                                                                       jclass,
                                                                                       jstring name) {
  core().setDeviceName(toStd(env, name));
}

extern "C" JNIEXPORT void JNICALL Java_com_owc_webcam_NativeBridge_nativeScreenModeChanged(JNIEnv*, jclass,
                                                                                           jint mode) {
  core().screenModeChanged(mode);
}

// ------------------------------------------------------------------ pairing
extern "C" JNIEXPORT jboolean JNICALL Java_com_owc_webcam_NativeBridge_nativeSetDataDir(JNIEnv* env, jclass,
                                                                                       jstring dir) {
  std::string err;
  if (core().setDataDir(toStd(env, dir), err)) return JNI_TRUE;
  LOGE_LOCAL("nativeSetDataDir failed: %s", err.c_str());
  return JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL Java_com_owc_webcam_NativeBridge_nativeGetPairedClients(JNIEnv* env,
                                                                                             jclass) {
  return env->NewStringUTF(core().pairedClientsJson().c_str());  // names are sanitized (modified-UTF-8 safe)
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_owc_webcam_NativeBridge_nativeUnpairClient(JNIEnv* env, jclass,
                                                                                         jstring idHex) {
  return core().unpairClient(toStd(env, idHex)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_owc_webcam_NativeBridge_nativeCancelPairing(JNIEnv*, jclass) {
  core().cancelPairing();
}
