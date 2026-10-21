package com.owc.webcam

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager

/**
 * Tela totalmente preta com brilho mínimo: em OLED equivale a "desligada" enquanto a
 * câmera continua transmitindo. Qualquer toque ou tecla (Back, volume...) sai; Home/Recents
 * (onStop) e tela apagada/bloqueada (ACTION_SCREEN_OFF) também. Fecha ainda ao receber o
 * broadcast do serviço (modo de tela normal). Toda saída iniciada aqui avisa o serviço
 * (requestExitBlack) para sincronizar o ParamStore/PC.
 */
class BlackScreenActivity : Activity() {

    private val closeReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            finish()
        }
    }
    // Segurança: se o serviço não estiver vivo para tratar ACTION_SCREEN_OFF, a activity
    // própria não pode ficar sobre a tela de bloqueio com brilho mínimo.
    private val screenOffReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            exitBlack()
        }
    }
    private var receiverRegistered = false
    private var screenOffRegistered = false

    private fun exitBlack() {
        WebcamService.requestExitBlack()
        if (!isFinishing) finish()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= 27) {
            setShowWhenLocked(true)
        } else {
            @Suppress("DEPRECATION")
            window.addFlags(WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED)
        }
        if (Build.VERSION.SDK_INT >= 28) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        setBrightness()

        val v = View(this)
        v.setBackgroundColor(Color.BLACK)
        setContentView(v)

        val f = IntentFilter(WebcamService.ACTION_CLOSE_BLACK)
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(closeReceiver, f, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(closeReceiver, f)
        }
        receiverRegistered = true
        registerReceiver(screenOffReceiver, IntentFilter(Intent.ACTION_SCREEN_OFF))
        screenOffRegistered = true
    }

    // Qualquer toque (ACTION_DOWN basta).
    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        if (ev.actionMasked == MotionEvent.ACTION_DOWN) exitBlack()
        return true
    }

    // Qualquer tecla (Back, volume, camera...). Home nunca chega aqui: ver onStop.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) exitBlack()
        return true
    }

    // Back por gesto (API 33+ sem key event) cai aqui.
    @Suppress("OVERRIDE_DEPRECATION")
    override fun onBackPressed() {
        exitBlack()
    }

    // Home / Recents / outro app / tela apagada: a activity deixa de ser visível.
    override fun onStop() {
        super.onStop()
        if (!isFinishing) exitBlack()
    }

    private fun setBrightness() {
        val lp = window.attributes
        // 0.0f = BRIGHTNESS_OVERRIDE_OFF (backlight no mínimo possível)
        lp.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_OFF
        window.attributes = lp
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
        setBrightness()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let {
                it.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION)
        }
    }

    override fun onDestroy() {
        if (receiverRegistered) {
            try { unregisterReceiver(closeReceiver) } catch (_: Throwable) {}
            receiverRegistered = false
        }
        if (screenOffRegistered) {
            try { unregisterReceiver(screenOffReceiver) } catch (_: Throwable) {}
            screenOffRegistered = false
        }
        super.onDestroy()
    }
}
