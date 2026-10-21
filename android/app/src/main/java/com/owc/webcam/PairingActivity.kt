package com.owc.webcam

import android.app.Activity
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.WindowManager
import android.widget.Button

/**
 * Código de pareamento em tela própria (estilo diálogo): aberta pela notificação
 * (toque ou full-screen intent), aparece sobre a tela de bloqueio e acende a tela.
 * Fecha sozinha quando o pareamento termina (PAIRING_ENDED) ou o código expira.
 */
class PairingActivity : Activity(), Pairing.Listener {

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var codeView: View

    private val timer = object : Runnable {
        override fun run() {
            val p = Pairing.pending
            if (p == null || p.secondsLeft() <= 0) {
                finish()
                return
            }
            Pairing.updateTimer(codeView, p)
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= 27) {
            setShowWhenLocked(true)
            setTurnScreenOn(true)
        } else {
            @Suppress("DEPRECATION")
            window.addFlags(WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON)
        }
        setFinishOnTouchOutside(false)
        setContentView(R.layout.activity_pairing)
        codeView = findViewById(R.id.pairCodeView)
        findViewById<Button>(R.id.btnPairCancel).setOnClickListener {
            NativeBridge.safeCancelPairing()
            finish()
        }
    }

    override fun onStart() {
        super.onStart()
        val p = Pairing.pending
        if (p == null) {
            finish()
            return
        }
        Pairing.activityVisible = true
        Pairing.addListener(this)
        Pairing.bindCodeView(codeView, p)
        handler.post(timer)
    }

    override fun onStop() {
        Pairing.activityVisible = false
        Pairing.removeListener(this)
        handler.removeCallbacks(timer)
        super.onStop()
    }

    override fun onPairingCode(p: Pairing.Pending) {
        Pairing.bindCodeView(codeView, p)
    }

    // Back só fecha a tela (padrão); o pareamento continua e a notificação mantém "Cancelar".
    override fun onPairingEnded(outcome: Int, pcName: String) {
        finish()
    }
}
