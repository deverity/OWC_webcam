package com.owc.webcam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.Typeface
import android.provider.Settings
import android.text.SpannableString
import android.text.Spanned
import android.text.style.RelativeSizeSpan
import android.text.style.StyleSpan
import android.widget.Toast
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import org.json.JSONObject
import java.util.Locale

/**
 * Serviço em primeiro plano (tipo camera) que mantém o núcleo nativo rodando,
 * independente da Activity e com a tela apagada.
 */
class WebcamService : Service() {

    companion object {
        private const val TAG = "OWC.Service"
        const val ACTION_START = "com.owc.webcam.action.START"
        const val ACTION_STOP = "com.owc.webcam.action.STOP"
        const val ACTION_CLOSE_BLACK = "com.owc.webcam.action.CLOSE_BLACK"
        const val ACTION_CANCEL_PAIRING = "com.owc.webcam.action.CANCEL_PAIRING"
        private const val CH_MAIN = "owc_stream"
        private const val CH_SCREEN = "owc_screen"
        private const val CH_PAIR = "owc_pairing"
        private const val NOTIF_ID = 1
        private const val NOTIF_SCREEN_ID = 2
        private const val NOTIF_PAIR_ID = 3

        @Volatile
        var running: Boolean = false
            private set

        @Volatile
        private var instance: WebcamService? = null

        /**
         * Pedido de saída da tela preta vindo do lado do celular (BlackScreenActivity).
         * Passa pelo mesmo caminho único do overlay; sem serviço vivo é no-op.
         */
        fun requestExitBlack() {
            val s = instance ?: return
            s.main.post { s.exitBlackScreen(syncNative = true) }
        }

        fun start(ctx: Context) {
            val i = Intent(ctx, WebcamService::class.java).setAction(ACTION_START)
            ctx.startForegroundService(i)
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, WebcamService::class.java))
        }
    }

    private val main = Handler(Looper.getMainLooper())
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var wifiLockLL: WifiManager.WifiLock? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var nativeStarted = false
    private var batteryReceiverRegistered = false
    private var thermalListener: Any? = null
    private var thermalStatus = 0
    private var lastText = ""
    private var lastBlackRequested = false

    private val pollRunnable = object : Runnable {
        override fun run() {
            updateNotification(buildStatusText())
            main.postDelayed(this, 2000)
        }
    }

    private val batteryReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent != null) pushBattery(intent)
        }
    }

    // ---------------------------------------------------------------- ciclo

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        NativeBridge.ensureDataDir(this)
        createChannels()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (intent?.action == ACTION_CANCEL_PAIRING) {
            // Ação "Cancelar" da notificação do código. Sem núcleo rodando a notificação é
            // resto de uma sessão anterior: só limpa.
            if (nativeStarted) {
                NativeBridge.safeCancelPairing()
                return START_STICKY
            }
            cancelPairNotification()
            if (!running) stopSelf()
            return START_NOT_STICKY
        }
        if (!goForeground()) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (!nativeStarted) startCore()
        return START_STICKY
    }

    private fun goForeground(): Boolean {
        if (checkSelfPermission(android.Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            Log.e(TAG, "sem permissão de câmera; não é possível iniciar")
            return false
        }
        val n = buildNotification(getString(R.string.notif_starting))
        return try {
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
            } else {
                startForeground(NOTIF_ID, n)
            }
            true
        } catch (t: Throwable) {
            Log.e(TAG, "startForeground falhou", t)
            false
        }
    }

    private fun startCore() {
        running = true
        acquireLocks()
        NativeBridge.eventListener = { type, arg, str -> main.post { handleEvent(type, arg, str) } }

        if (!NativeBridge.available) {
            updateNotification(getString(R.string.native_missing))
            Log.e(TAG, "biblioteca nativa ausente: ${NativeBridge.loadError}")
            // mantém o serviço de pé para o usuário ver o aviso
            return
        }
        val p = Prefs(this)
        NativeBridge.ensureDataDir(this)  // pareamentos: obrigatório antes do nativeStart
        val ok = NativeBridge.safeStart(
            p.deviceName, Build.MODEL, Build.VERSION.SDK_INT,
            p.controlPort, p.udpPort, p.discoveryPort
        )
        if (!ok) {
            Log.e(TAG, "nativeStart retornou false")
            updateNotification(getString(R.string.start_failed))
            stopSelf()
            return
        }
        nativeStarted = true
        registerBattery()
        registerThermal()
        main.postDelayed(pollRunnable, 2000)
        updateNotification(getString(R.string.notif_waiting))
    }

    override fun onDestroy() {
        running = false
        instance = null
        unregisterScreenOff()
        main.removeCallbacksAndMessages(null)
        NativeBridge.eventListener = null
        if (batteryReceiverRegistered) {
            try { unregisterReceiver(batteryReceiver) } catch (_: Throwable) {}
            batteryReceiverRegistered = false
        }
        unregisterThermal()
        if (nativeStarted) {
            NativeBridge.safeStop()
            nativeStarted = false
        }
        hideOverlay()
        releaseLocks()
        sendBroadcast(Intent(ACTION_CLOSE_BLACK).setPackage(packageName))
        getSystemService(NotificationManager::class.java).cancel(NOTIF_SCREEN_ID)
        // O listener já foi removido: o PAIRING_ENDED do stop nativo não chega aqui.
        cancelPairNotification()
        Pairing.onEnded(NativeBridge.PAIR_DISCONNECTED, null)
        super.onDestroy()
    }

    // ---------------------------------------------------------------- locks

    private fun acquireLocks() {
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "owc:stream").apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (t: Throwable) { Log.w(TAG, "wakelock", t) }
        try {
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            if (Build.VERSION.SDK_INT >= 29) {
                wifiLockLL = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "owc:wifi-ll").apply {
                    setReferenceCounted(false)
                    acquire()
                }
            }
            // Mantem o radio sem power-save com a tela apagada (LOW_LATENCY so vale com tela ligada)
            @Suppress("DEPRECATION")
            wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "owc:wifi").apply {
                setReferenceCounted(false)
                acquire()
            }
            multicastLock = wm.createMulticastLock("owc:mcast").apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (t: Throwable) { Log.w(TAG, "wifi locks", t) }
    }

    private fun releaseLocks() {
        try { wakeLock?.takeIf { it.isHeld }?.release() } catch (_: Throwable) {}
        try { wifiLock?.takeIf { it.isHeld }?.release() } catch (_: Throwable) {}
        try { wifiLockLL?.takeIf { it.isHeld }?.release() } catch (_: Throwable) {}
        try { multicastLock?.takeIf { it.isHeld }?.release() } catch (_: Throwable) {}
        wakeLock = null; wifiLock = null; wifiLockLL = null; multicastLock = null
    }

    // ---------------------------------------------------- bateria / térmico

    private fun registerBattery() {
        val f = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
        val sticky = if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(batteryReceiver, f, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(batteryReceiver, f)
        }
        batteryReceiverRegistered = true
        if (sticky != null) pushBattery(sticky)
    }

    private fun pushBattery(i: Intent) {
        val level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
        val pct = if (level >= 0 && scale > 0) level * 100 / scale else -1
        val tempC = i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10f
        val st = i.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        val charging = st == BatteryManager.BATTERY_STATUS_CHARGING || st == BatteryManager.BATTERY_STATUS_FULL
        if (nativeStarted) NativeBridge.safeUpdateDevice(pct, tempC, charging, thermalStatus)
        lastBattery = i
    }

    private var lastBattery: Intent? = null

    private fun registerThermal() {
        if (Build.VERSION.SDK_INT < 29) return
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            thermalStatus = pm.currentThermalStatus
            val l = PowerManager.OnThermalStatusChangedListener { status ->
                thermalStatus = status
                lastBattery?.let { pushBattery(it) }
            }
            pm.addThermalStatusListener(mainExecutor, l)
            thermalListener = l
            lastBattery?.let { pushBattery(it) }
        } catch (t: Throwable) { Log.w(TAG, "thermal", t) }
    }

    private fun unregisterThermal() {
        if (Build.VERSION.SDK_INT < 29) return
        try {
            val l = thermalListener as? PowerManager.OnThermalStatusChangedListener ?: return
            (getSystemService(Context.POWER_SERVICE) as PowerManager).removeThermalStatusListener(l)
        } catch (_: Throwable) {}
        thermalListener = null
    }

    // --------------------------------------------------------------- eventos

    private fun handleEvent(type: Int, arg: Int, str: String?) {
        when (type) {
            NativeBridge.EVT_SCREEN_MODE -> {
                // Modo 0 veio do próprio PC: ele já sabe, não precisa sincronizar de volta.
                if (arg == 1) openBlackScreen() else exitBlackScreen(syncNative = false)
            }
            NativeBridge.EVT_DEVICE_NAME -> {
                val n = str?.trim().orEmpty()
                if (n.isNotEmpty()) Prefs(this).deviceName = n
            }
            NativeBridge.EVT_ERROR -> {
                Log.e(TAG, "erro nativo: $str")
                updateNotification(getString(R.string.notif_error, str ?: "?"))
            }
            NativeBridge.EVT_PAIRING_CODE -> {
                val p = Pairing.onCodeEvent(str, arg) ?: return
                // O código precisa ficar visível: sai da tela preta (se ativa) e, nesse caso, abre a
                // PairingActivity (permitido: a BlackScreenActivity ainda está em primeiro plano ou o
                // app tem a permissão de sobreposição usada pelo overlay). Fora disso: notificação
                // heads-up + diálogo da MainActivity se ela estiver visível.
                val wasBlack = lastBlackRequested || overlay != null
                exitBlackScreen(syncNative = true)
                showPairNotification(p, arg)
                if (wasBlack) {
                    try { startActivity(pairingIntent()) } catch (t: Throwable) { Log.w(TAG, "PairingActivity: $t") }
                }
                updateNotification(buildStatusText())
            }
            NativeBridge.EVT_PAIRING_ENDED -> {
                cancelPairNotification()
                Pairing.onEnded(arg, str)
                when (arg) {
                    NativeBridge.PAIR_OK ->
                        Toast.makeText(this, getString(R.string.paired_toast, str ?: "PC"), Toast.LENGTH_LONG).show()
                    NativeBridge.PAIR_FAILED ->
                        Toast.makeText(this, R.string.pair_failed_toast, Toast.LENGTH_LONG).show()
                }
                updateNotification(buildStatusText())
            }
            else -> {
                // Tela preta nao fica presa quando o PC encerra de proposito:
                //  - Bye (arg=1): desconexao explicita;
                //  - STREAMING_STOPPED com arg=0: PC parou a transmissao com a sessao de pe
                //    (arg=1 e o stop interno de uma sessao que caiu: perda, nao sai).
                // Conexao PERDIDA (arg=0, sem Bye): mantem preta; o PC reconecta sozinho
                // e o usuario sai com qualquer toque/tecla.
                if ((type == NativeBridge.EVT_CLIENT_DISCONNECTED && arg == 1) ||
                    (type == NativeBridge.EVT_STREAMING_STOPPED && arg == 0)) {
                    exitBlackScreen(syncNative = true)
                }
                updateNotification(buildStatusText())
            }
        }
    }

    private fun blackIntent(): Intent =
        Intent(this, BlackScreenActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)

    private var overlay: View? = null

    /**
     * View do overlay. Cobertura de saida (todas via [exitBlackScreen] com sync nativo):
     *  - toque: ACTION_DOWN em dispatchTouchEvent;
     *  - teclas (Back, volume, camera...): dispatchKeyEvent, possivel porque a janela e focavel;
     *  - Home/Recents/barra de notificacoes: Home nao chega como tecla; o que funciona e a janela
     *    focavel perder o foco (onWindowFocusChanged(false)), so valido depois de ter ganho foco.
     */
    private inner class BlackOverlayView(ctx: Context) : View(ctx) {
        private var hadFocus = false

        override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
            if (ev.actionMasked == MotionEvent.ACTION_DOWN) exitBlackScreen(syncNative = true)
            return true
        }

        override fun dispatchKeyEvent(event: KeyEvent): Boolean {
            if (event.action == KeyEvent.ACTION_DOWN) exitBlackScreen(syncNative = true)
            return true
        }

        override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
            super.onWindowFocusChanged(hasWindowFocus)
            if (hasWindowFocus) hadFocus = true
            else if (hadFocus) exitBlackScreen(syncNative = true)
        }
    }

    private fun showOverlay(): Boolean {
        if (overlay != null) return true
        if (!Settings.canDrawOverlays(this)) return false
        return try {
            val wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
            val v = BlackOverlayView(this)
            v.setBackgroundColor(Color.BLACK)
            v.isFocusable = true
            v.isFocusableInTouchMode = true
            @Suppress("DEPRECATION")
            v.systemUiVisibility = (View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION)
            // Sem FLAG_NOT_FOCUSABLE para receber teclas e foco; ALT_FOCUSABLE_IM evita
            // que a janela mexa no teclado (IME).
            val lp = WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
                        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
                        WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS or
                        WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM or
                        WindowManager.LayoutParams.FLAG_FULLSCREEN,
                PixelFormat.OPAQUE
            )
            lp.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_OFF
            if (Build.VERSION.SDK_INT >= 28) {
                lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
            wm.addView(v, lp)
            overlay = v
            v.requestFocus()
            true
        } catch (t: Throwable) {
            Log.w(TAG, "overlay falhou", t)
            false
        }
    }

    private fun hideOverlay() {
        val v = overlay ?: return
        overlay = null
        try { (getSystemService(Context.WINDOW_SERVICE) as WindowManager).removeView(v) } catch (_: Throwable) {}
    }

    // Receiver ativo so enquanto a tela preta estiver ligada.
    //  - ACTION_SCREEN_OFF: aparelho bloqueado/apagado (botao power) -> sai, para que ao
    //    desbloquear a tela esteja normal (cobre overlay e activity).
    //  - ACTION_CLOSE_SYSTEM_DIALOGS (so API < 31; depois e ignorado para apps): Home/Recents.
    //    Na pratica o onWindowFocusChanged do overlay ja cobre.
    private val blackReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            exitBlackScreen(syncNative = true)
        }
    }
    private var blackReceiverRegistered = false

    private fun registerScreenOff() {
        if (blackReceiverRegistered) return
        val f = IntentFilter(Intent.ACTION_SCREEN_OFF)
        if (Build.VERSION.SDK_INT < 31) f.addAction(Intent.ACTION_CLOSE_SYSTEM_DIALOGS)
        try {
            if (Build.VERSION.SDK_INT >= 33) {
                registerReceiver(blackReceiver, f, Context.RECEIVER_NOT_EXPORTED)
            } else {
                registerReceiver(blackReceiver, f)
            }
            blackReceiverRegistered = true
        } catch (t: Throwable) { Log.w(TAG, "receiver tela preta", t) }
    }

    private fun unregisterScreenOff() {
        if (!blackReceiverRegistered) return
        blackReceiverRegistered = false
        try { unregisterReceiver(blackReceiver) } catch (_: Throwable) {}
    }

    private fun openBlackScreen() {
        lastBlackRequested = true
        registerScreenOff()
        if (showOverlay()) return
        // Tentativa direta (funciona se o app estiver visível/permitido)
        try { startActivity(blackIntent()) } catch (t: Throwable) { Log.w(TAG, "startActivity tela preta: $t") }
        // Fallback para Android 10+: notificação com full-screen intent
        try {
            val pi = PendingIntent.getActivity(this, 1, blackIntent(), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            val b = Notification.Builder(this, CH_SCREEN)
                .setSmallIcon(R.drawable.ic_stat)
                .setContentTitle(getString(R.string.notif_title))
                .setContentText(getString(R.string.notif_black))
                .setCategory(Notification.CATEGORY_CALL)
                .setAutoCancel(true)
                .setContentIntent(pi)
                .setFullScreenIntent(pi, true)
            getSystemService(NotificationManager::class.java).notify(NOTIF_SCREEN_ID, b.build())
        } catch (t: Throwable) { Log.w(TAG, "full-screen notif", t) }
    }

    /**
     * Unico ponto de saida da tela preta (overlay + activity + notificacao + receiver).
     * Idempotente. syncNative=true quando a saida nao foi pedida pelo PC: avisa o nucleo
     * para zerar ScreenMode e atualizar a UI do PC.
     */
    private fun exitBlackScreen(syncNative: Boolean) {
        val wasActive = lastBlackRequested || overlay != null
        lastBlackRequested = false
        unregisterScreenOff()
        hideOverlay()
        sendBroadcast(Intent(ACTION_CLOSE_BLACK).setPackage(packageName))
        getSystemService(NotificationManager::class.java).cancel(NOTIF_SCREEN_ID)
        if (wasActive && syncNative && nativeStarted) NativeBridge.safeScreenModeChanged(0)
    }

    // ----------------------------------------------------------- notificação

    private fun createChannels() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CH_MAIN, getString(R.string.notif_channel), NotificationManager.IMPORTANCE_LOW)
                .apply { setShowBadge(false) }
        )
        nm.createNotificationChannel(
            NotificationChannel(CH_SCREEN, getString(R.string.notif_channel_screen), NotificationManager.IMPORTANCE_HIGH)
                .apply { setShowBadge(false); setSound(null, null) }
        )
        // Alta importância = heads-up; público = código visível na tela de bloqueio.
        nm.createNotificationChannel(
            NotificationChannel(CH_PAIR, getString(R.string.pair_notif_channel), NotificationManager.IMPORTANCE_HIGH)
                .apply {
                    setShowBadge(false)
                    enableVibration(true)
                    lockscreenVisibility = Notification.VISIBILITY_PUBLIC
                }
        )
    }

    // ------------------------------------------------------------- pareamento

    private fun pairingIntent(): Intent =
        Intent(this, PairingActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)

    private fun canUseFullScreenIntent(): Boolean =
        Build.VERSION.SDK_INT < 34 || getSystemService(NotificationManager::class.java).canUseFullScreenIntent()

    /**
     * Código em notificação heads-up (canal de alta importância, visível na tela de bloqueio) com
     * "Cancelar". O full-screen intent abre a PairingActivity sobre a tela de bloqueio / acende a
     * tela quando o sistema permite (Android 14+: só com a permissão de full-screen concedida).
     */
    private fun showPairNotification(p: Pairing.Pending, seconds: Int) {
        try {
            val open = PendingIntent.getActivity(
                this, 3, pairingIntent(), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
            val cancel = PendingIntent.getService(
                this, 4, Intent(this, WebcamService::class.java).setAction(ACTION_CANCEL_PAIRING),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
            val code = Pairing.formatCode(p.code)
            val big = SpannableString(code + "\n" + getString(R.string.pair_notif_text, p.pcName)).apply {
                setSpan(RelativeSizeSpan(2.0f), 0, code.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                setSpan(StyleSpan(Typeface.BOLD), 0, code.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
            val b = Notification.Builder(this, CH_PAIR)
                .setSmallIcon(R.drawable.ic_stat)
                .setContentTitle(getString(R.string.pair_notif_title, code))
                .setContentText(getString(R.string.pair_notif_text, p.pcName))
                .setStyle(Notification.BigTextStyle().bigText(big))
                .setCategory(Notification.CATEGORY_CALL)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setOngoing(true)
                .setAutoCancel(false)
                .setOnlyAlertOnce(true)
                .setTimeoutAfter(seconds.coerceAtLeast(1) * 1000L)
                .setContentIntent(open)
                .addAction(Notification.Action.Builder(null, getString(R.string.cancel), cancel).build())
            if (canUseFullScreenIntent()) b.setFullScreenIntent(open, true)
            getSystemService(NotificationManager::class.java).notify(NOTIF_PAIR_ID, b.build())
        } catch (t: Throwable) { Log.w(TAG, "notificação de pareamento", t) }
    }

    private fun cancelPairNotification() {
        try { getSystemService(NotificationManager::class.java).cancel(NOTIF_PAIR_ID) } catch (_: Throwable) {}
    }

    private fun buildNotification(text: String): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val stop = PendingIntent.getService(
            this, 2, Intent(this, WebcamService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CH_MAIN)
            .setSmallIcon(R.drawable.ic_stat)
            .setContentTitle(getString(R.string.notif_title))
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, getString(R.string.notif_stop), stop).build())
            .build()
    }

    private fun updateNotification(text: String) {
        if (text == lastText) return
        lastText = text
        try {
            getSystemService(NotificationManager::class.java).notify(NOTIF_ID, buildNotification(text))
        } catch (t: Throwable) { Log.w(TAG, "notify", t) }
    }

    private fun buildStatusText(): String {
        val s = NativeBridge.safeStatus() ?: return getString(R.string.notif_waiting)
        return try {
            val j = JSONObject(s)
            val state = j.optString("state", "idle")
            val peer = j.optString("peer", "")
            when (state) {
                "streaming" -> String.format(
                    Locale.US, "%s - %dx%d @ %.0f fps - %s",
                    peer, j.optInt("width"), j.optInt("height"), j.optDouble("fps", 0.0), j.optString("codec", "")
                )
                "connected" -> getString(R.string.state_connected) + " - " + peer
                "pairing" -> getString(R.string.state_pairing) + " - " + peer
                else -> getString(R.string.notif_waiting)
            }
        } catch (t: Throwable) {
            getString(R.string.notif_waiting)
        }
    }
}
