package com.owc.webcam

import android.content.Context
import android.os.SystemClock
import android.view.LayoutInflater
import android.view.View
import android.widget.TextView
import org.json.JSONArray

/**
 * Estado do pareamento em andamento, alimentado pelo WebcamService a partir dos eventos
 * nativos PAIRING_CODE / PAIRING_ENDED. Tudo na thread principal.
 * O código nunca vai para o log.
 */
object Pairing {

    class Pending(val code: String, val pcName: String, val expiresAt: Long /* elapsedRealtime ms */) {
        fun secondsLeft(): Int = ((expiresAt - SystemClock.elapsedRealtime() + 999) / 1000).toInt().coerceAtLeast(0)
    }

    interface Listener {
        fun onPairingCode(p: Pending)
        fun onPairingEnded(outcome: Int, pcName: String)
    }

    class PairedPc(val id: String, val name: String, val pairedAt: Long /* unix s */)

    var pending: Pending? = null
        private set

    /** PairingActivity visível: a MainActivity não abre um segundo diálogo. */
    var activityVisible = false

    private val listeners = LinkedHashSet<Listener>()

    fun addListener(l: Listener) { listeners.add(l) }
    fun removeListener(l: Listener) { listeners.remove(l) }

    /** str do evento PAIRING_CODE: "código\nnome". */
    fun onCodeEvent(str: String?, seconds: Int): Pending? {
        val s = str ?: return null
        val nl = s.indexOf('\n')
        val code = if (nl >= 0) s.substring(0, nl) else s
        val name = if (nl >= 0) s.substring(nl + 1) else ""
        if (code.isEmpty()) return null
        val p = Pending(code, name.ifEmpty { "PC" }, SystemClock.elapsedRealtime() + seconds.coerceAtLeast(1) * 1000L)
        pending = p
        for (l in listeners.toList()) l.onPairingCode(p)
        return p
    }

    /** Também chamado sem nada pendente (ex.: serviço parando): os diálogos fecham do mesmo jeito. */
    fun onEnded(outcome: Int, name: String?) {
        pending = null
        for (l in listeners.toList()) l.onPairingEnded(outcome, name.orEmpty())
    }

    /** "123456" -> "123 456" (mais fácil de ler e digitar). */
    fun formatCode(code: String): String =
        if (code.length == 6) code.substring(0, 3) + " " + code.substring(3) else code

    fun parseList(json: String?): List<PairedPc> {
        if (json.isNullOrEmpty()) return emptyList()
        return try {
            val a = JSONArray(json)
            (0 until a.length()).map {
                val o = a.getJSONObject(it)
                PairedPc(o.optString("id"), o.optString("name", "PC"), o.optLong("pairedAt"))
            }.sortedByDescending { it.pairedAt }
        } catch (_: Throwable) {
            emptyList()
        }
    }

    // ---- view compartilhada (diálogo da MainActivity e PairingActivity)

    fun inflateCodeView(ctx: Context): View = LayoutInflater.from(ctx).inflate(R.layout.pairing_code, null)

    fun bindCodeView(v: View, p: Pending) {
        v.findViewById<TextView>(R.id.txtPairCode).text = formatCode(p.code)
        v.findViewById<TextView>(R.id.txtPairPc).text = v.context.getString(R.string.pair_pc, p.pcName)
        updateTimer(v, p)
    }

    fun updateTimer(v: View, p: Pending) {
        val left = p.secondsLeft()
        v.findViewById<TextView>(R.id.txtPairTimer).text =
            v.context.getString(R.string.pair_expires, left / 60, left % 60)
    }
}
