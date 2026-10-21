package com.owc.webcam

import android.content.Context
import android.content.SharedPreferences
import android.os.Build

/** Preferências simples (SharedPreferences). */
class Prefs(ctx: Context) {
    private val sp: SharedPreferences = ctx.applicationContext.getSharedPreferences("owc", Context.MODE_PRIVATE)

    var deviceName: String
        get() = sp.getString("device_name", null)?.takeIf { it.isNotBlank() } ?: Build.MODEL
        set(v) = sp.edit().putString("device_name", v).apply()

    var controlPort: Int
        get() = sp.getInt("control_port", 47800)
        set(v) = sp.edit().putInt("control_port", v).apply()

    var udpPort: Int
        get() = sp.getInt("udp_port", 47801)
        set(v) = sp.edit().putInt("udp_port", v).apply()

    var discoveryPort: Int
        get() = sp.getInt("discovery_port", 47802)
        set(v) = sp.edit().putInt("discovery_port", v).apply()

    var keepScreenOn: Boolean
        get() = sp.getBoolean("keep_screen_on", false)
        set(v) = sp.edit().putBoolean("keep_screen_on", v).apply()
}
