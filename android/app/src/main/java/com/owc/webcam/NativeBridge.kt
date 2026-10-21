package com.owc.webcam

import android.content.Context
import android.util.Log
import android.view.Surface

/**
 * Ponte JNI para libowc_native.so (contrato em docs/ARCHITECTURE.md).
 * Se a biblioteca não carregar, [available] = false e [loadError] explica; nenhum
 * chamador deve invocar os métodos external nesse caso (use os wrappers safe*).
 */
object NativeBridge {
    private const val TAG = "OWC.Native"

    const val EVT_CLIENT_CONNECTED = 1
    const val EVT_CLIENT_DISCONNECTED = 2
    const val EVT_SCREEN_MODE = 3
    const val EVT_STREAMING_STARTED = 4
    const val EVT_STREAMING_STOPPED = 5
    const val EVT_ERROR = 6
    const val EVT_DEVICE_NAME = 7
    /** Um PC pediu pareamento: int = validade do código (s), str = "código" + '\n' + "nome do PC". */
    const val EVT_PAIRING_CODE = 8
    /** O código deixou de valer: int = PAIR_* (resultado), str = nome do PC. */
    const val EVT_PAIRING_ENDED = 9

    const val PAIR_OK = 0
    const val PAIR_FAILED = 1
    const val PAIR_EXPIRED = 2
    const val PAIR_REJECTED = 3
    const val PAIR_DISCONNECTED = 4

    @JvmField
    @Volatile
    var available: Boolean = false

    @JvmField
    @Volatile
    var loadError: String? = null

    /** Receptor dos eventos nativos (chamado em threads nativas!). */
    @Volatile
    var eventListener: ((type: Int, intArg: Int, strArg: String?) -> Unit)? = null

    init {
        try {
            System.loadLibrary("owc_native")
            available = true
        } catch (t: Throwable) {
            available = false
            val m = t.message ?: t.javaClass.simpleName
            loadError = if (m.contains("couldn't find")) "libowc_native.so não encontrada" else m.take(160)
            Log.w(TAG, "libowc_native indisponível: $loadError")
        }
    }

    @JvmStatic
    external fun nativeStart(
        deviceName: String, model: String, apiLevel: Int,
        controlPort: Int, udpPort: Int, discoveryPort: Int
    ): Boolean

    @JvmStatic
    external fun nativeStop()

    @JvmStatic
    external fun nativeSetPreviewSurface(surface: Surface?)

    @JvmStatic
    external fun nativeGetStatus(): String

    @JvmStatic
    external fun nativeUpdateDeviceStatus(
        batteryPct: Int, batteryTempC: Float, charging: Boolean, thermalStatus: Int
    )

    @JvmStatic
    external fun nativeSetDeviceName(name: String)

    /** O usuário saiu da tela preta no celular: sincroniza o ParamStore/PC. */
    @JvmStatic
    external fun nativeScreenModeChanged(mode: Int)

    /** Diretório dos pareamentos; obrigatório antes de nativeStart. Funciona com o núcleo parado. */
    @JvmStatic
    external fun nativeSetDataDir(path: String): Boolean

    /** JSON: [{"id":"hex","name":"...","pairedAt":segundos_unix}]. Funciona com o núcleo parado. */
    @JvmStatic
    external fun nativeGetPairedClients(): String

    /** Remove o PC (id hex) ou todos (""); derruba a sessão se ele estiver conectado. */
    @JvmStatic
    external fun nativeUnpairClient(idHex: String): Boolean

    /** O usuário cancelou o código no celular: o PC recebe Rejected e a conexão é fechada. */
    @JvmStatic
    external fun nativeCancelPairing()

    /** Chamado pelo código nativo, em qualquer thread. */
    @JvmStatic
    fun onNativeEvent(type: Int, intArg: Int, strArg: String?) {
        try {
            eventListener?.invoke(type, intArg, strArg)
        } catch (t: Throwable) {
            Log.e(TAG, "erro no listener de evento", t)
        }
    }

    // ---- wrappers seguros (nunca lançam) ----

    fun safeStart(name: String, model: String, api: Int, c: Int, u: Int, d: Int): Boolean {
        if (!available) return false
        return try { nativeStart(name, model, api, c, u, d) } catch (t: Throwable) { Log.e(TAG, "nativeStart", t); false }
    }

    fun safeStop() {
        if (!available) return
        try { nativeStop() } catch (t: Throwable) { Log.e(TAG, "nativeStop", t) }
    }

    fun safeSetPreview(s: Surface?) {
        if (!available) return
        try { nativeSetPreviewSurface(s) } catch (t: Throwable) { Log.e(TAG, "nativeSetPreviewSurface", t) }
    }

    fun safeStatus(): String? {
        if (!available) return null
        return try { nativeGetStatus() } catch (t: Throwable) { null }
    }

    fun safeUpdateDevice(pct: Int, tempC: Float, charging: Boolean, thermal: Int) {
        if (!available) return
        try { nativeUpdateDeviceStatus(pct, tempC, charging, thermal) } catch (t: Throwable) { Log.e(TAG, "nativeUpdateDeviceStatus", t) }
    }

    fun safeSetName(name: String) {
        if (!available) return
        try { nativeSetDeviceName(name) } catch (t: Throwable) { Log.e(TAG, "nativeSetDeviceName", t) }
    }

    fun safeScreenModeChanged(mode: Int) {
        if (!available) return
        try { nativeScreenModeChanged(mode) } catch (t: Throwable) { Log.e(TAG, "nativeScreenModeChanged", t) }
    }

    @Volatile
    private var dataDirSet = false

    /** Idempotente; chame antes de qualquer outro uso nativo (serviço e activity). */
    fun ensureDataDir(ctx: Context): Boolean {
        if (dataDirSet) return true
        if (!available) return false
        val ok = try {
            nativeSetDataDir(ctx.applicationContext.filesDir.absolutePath)
        } catch (t: Throwable) {
            Log.e(TAG, "nativeSetDataDir", t); false
        }
        dataDirSet = ok
        return ok
    }

    fun safeGetPairedClients(): String? {
        if (!available) return null
        return try { nativeGetPairedClients() } catch (t: Throwable) { Log.e(TAG, "nativeGetPairedClients", t); null }
    }

    fun safeUnpairClient(idHex: String): Boolean {
        if (!available) return false
        return try { nativeUnpairClient(idHex) } catch (t: Throwable) { Log.e(TAG, "nativeUnpairClient", t); false }
    }

    fun safeCancelPairing() {
        if (!available) return
        try { nativeCancelPairing() } catch (t: Throwable) { Log.e(TAG, "nativeCancelPairing", t) }
    }
}
