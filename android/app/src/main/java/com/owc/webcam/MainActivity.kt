package com.owc.webcam

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.Locale

class MainActivity : Activity() {

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var prefs: Prefs
    private lateinit var txtStatus: TextView
    private lateinit var txtNetwork: TextView
    private lateinit var editName: EditText
    private lateinit var btnService: Button
    private lateinit var btnBattery: Button
    private lateinit var btnOverlay: Button
    private lateinit var txtUsbState: TextView
    private lateinit var txtUsbWarn: TextView
    private var tick = 0
    private lateinit var swPreview: Switch
    private lateinit var swKeepOn: Switch
    private lateinit var surface: SurfaceView

    private var visible = false
    private var pendingStart = false
    private var pendingAutostart = false

    private val poll = object : Runnable {
        override fun run() {
            refreshStatus()
            if (tick++ % 4 == 0) refreshNetwork()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        prefs = Prefs(this)

        txtStatus = findViewById(R.id.txtStatus)
        txtNetwork = findViewById(R.id.txtNetwork)
        editName = findViewById(R.id.editName)
        btnService = findViewById(R.id.btnService)
        btnBattery = findViewById(R.id.btnBattery)
        swPreview = findViewById(R.id.swPreview)
        swKeepOn = findViewById(R.id.swKeepOn)
        surface = findViewById(R.id.surfacePreview)

        editName.setText(prefs.deviceName)
        findViewById<Button>(R.id.btnApplyName).setOnClickListener { applyName() }
        btnService.setOnClickListener {
            if (WebcamService.running) stopService() else requestAndStart()
        }
        findViewById<Button>(R.id.btnBlack).setOnClickListener {
            startActivity(Intent(this, BlackScreenActivity::class.java))
        }
        btnBattery.setOnClickListener { requestBatteryExemption() }
        txtUsbState = findViewById(R.id.txtUsbState)
        txtUsbWarn = findViewById(R.id.txtUsbWarn)
        findViewById<Button>(R.id.btnUsb).setOnClickListener { openTetherSettings() }
        findViewById<Button>(R.id.btnHelp).setOnClickListener {
            AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert)
                .setTitle(R.string.help_title)
                .setMessage(R.string.help_body)
                .setPositiveButton(R.string.close, null)
                .show()
        }
        btnOverlay = findViewById(R.id.btnOverlay)
        btnOverlay.setOnClickListener {
            try {
                startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")))
            } catch (_: Throwable) {}
        }

        swKeepOn.isChecked = prefs.keepScreenOn
        applyKeepOn(prefs.keepScreenOn)
        swKeepOn.setOnCheckedChangeListener { _, on ->
            prefs.keepScreenOn = on
            applyKeepOn(on)
        }

        surface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                NativeBridge.safeSetPreview(holder.surface)
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                NativeBridge.safeSetPreview(null)
            }
        })
        // preview desligado por padrão (economiza energia)
        swPreview.isChecked = false
        swPreview.setOnCheckedChangeListener { _, on ->
            if (on) {
                surface.visibility = View.VISIBLE   // surfaceCreated dispara
            } else {
                NativeBridge.safeSetPreview(null)
                surface.visibility = View.GONE      // surfaceDestroyed dispara
            }
        }

        refreshNetwork()
        handleIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(i: Intent?) {
        if (i?.getBooleanExtra("autostart", false) == true) {
            i.removeExtra("autostart")
            pendingAutostart = true
            if (hasPermissions()) {
                pendingAutostart = false
                WebcamService.start(this)
                moveTaskToBack(true)
            } else {
                requestPermissionsNow()
            }
        }
    }

    override fun onStart() {
        super.onStart()
        visible = true
        refreshNetwork()
        refreshStatus()
        handler.post(poll)
    }

    override fun onStop() {
        visible = false
        handler.removeCallbacks(poll)
        super.onStop()
    }

    // ------------------------------------------------------------- serviço

    private fun hasPermissions(): Boolean =
        checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    private fun requestPermissionsNow() {
        val list = ArrayList<String>()
        if (!hasPermissions()) list.add(Manifest.permission.CAMERA)
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) list.add(Manifest.permission.POST_NOTIFICATIONS)
        if (list.isNotEmpty()) requestPermissions(list.toTypedArray(), 100)
    }

    private fun requestAndStart() {
        if (hasPermissions()) {
            WebcamService.start(this)
        } else {
            pendingStart = true
            requestPermissionsNow()
        }
    }

    private fun stopService() {
        WebcamService.stop(this)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != 100) return
        if (hasPermissions()) {
            if (pendingStart || pendingAutostart) {
                val auto = pendingAutostart
                pendingStart = false
                pendingAutostart = false
                WebcamService.start(this)
                if (auto) moveTaskToBack(true)
            }
        } else {
            pendingStart = false
            pendingAutostart = false
            Toast.makeText(this, R.string.perm_denied, Toast.LENGTH_LONG).show()
        }
    }

    // ---------------------------------------------------------------- opções

    private fun applyName() {
        val n = editName.text.toString().trim().ifEmpty { Build.MODEL }
        editName.setText(n)
        prefs.deviceName = n
        NativeBridge.safeSetName(n)
        (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager).hideSoftInputFromWindow(editName.windowToken, 0)
        editName.clearFocus()
    }

    private fun applyKeepOn(on: Boolean) {
        if (on) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    private fun isIgnoringBattery(): Boolean =
        (getSystemService(POWER_SERVICE) as PowerManager).isIgnoringBatteryOptimizations(packageName)

    private fun requestBatteryExemption() {
        try {
            if (isIgnoringBattery()) {
                startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
            } else {
                startActivity(
                    Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
                        .setData(Uri.parse("package:$packageName"))
                )
            }
        } catch (t: Throwable) {
            try { startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)) } catch (_: Throwable) {}
        }
    }

    // ------------------------------------------------------------------ UI

    private fun openTetherSettings() {
        val candidates = listOf(
            Intent().setComponent(ComponentName("com.android.settings", "com.android.settings.TetherSettings")),
            Intent("android.settings.TETHER_SETTINGS"),
            Intent().setComponent(ComponentName("com.android.settings", "com.android.settings.Settings\$TetherSettingsActivity")),
            Intent(Settings.ACTION_WIRELESS_SETTINGS)
        )
        for (i in candidates) {
            try {
                startActivity(i)
                return
            } catch (_: Throwable) {
                // tenta o proximo
            }
        }
        Toast.makeText(this, R.string.usb_toast_fail, Toast.LENGTH_LONG).show()
    }

    private fun isUsbIf(n: String) = n.startsWith("rndis") || n.startsWith("usb") || n.startsWith("ncm")

    private fun netRank(n: String): Int = when {
        isUsbIf(n) -> -1
        n.startsWith("wlan") || n.startsWith("ap") || n.startsWith("swlan") || n.startsWith("wifi") -> 0
        n.startsWith("eth") || n.startsWith("en") -> 2
        n.startsWith("rmnet") || n.startsWith("ccmni") || n.startsWith("pdp") || n.startsWith("v4-rmnet") ||
            n.startsWith("clat") || n.startsWith("ppp") || n.startsWith("tun") -> 9
        else -> 5
    }

    private fun refreshNetwork() {
        val rows = ArrayList<Triple<Int, String, String>>()
        try {
            val ifs = NetworkInterface.getNetworkInterfaces()
            while (ifs != null && ifs.hasMoreElements()) {
                val ni = ifs.nextElement()
                if (!ni.isUp || ni.isLoopback) continue
                for (a in ni.inetAddresses) {
                    if (a is Inet4Address && !a.isLoopbackAddress) rows.add(Triple(netRank(ni.name), ni.name, a.hostAddress ?: ""))
                }
            }
        } catch (_: Throwable) {}
        rows.sortBy { it.first }
        val sb = StringBuilder()
        var usable = false
        var usbIp: String? = null
        for ((rank, name, addr) in rows) {
            if (isUsbIf(name)) {
                usbIp = addr
                sb.append(getString(R.string.net_usb_label)).append("  ").append(addr)
            } else {
                sb.append(name).append("  ").append(addr)
            }
            if (rank >= 9) sb.append("  ").append(getString(R.string.net_mobile)) else usable = true
            sb.append('\n')
        }
        if (rows.isEmpty()) sb.append(getString(R.string.no_network)).append('\n')
        if (!usable) sb.append(getString(R.string.net_hint)).append('\n')
        sb.append(getString(R.string.ports_fmt, prefs.controlPort, prefs.udpPort, prefs.discoveryPort))
        txtNetwork.text = sb.toString()
        if (usbIp != null) {
            txtUsbState.text = getString(R.string.usb_active, usbIp)
            txtUsbState.setTextColor(0xFF81C784.toInt())
            txtUsbWarn.visibility = View.VISIBLE
        } else {
            txtUsbState.text = getString(R.string.usb_inactive)
            txtUsbState.setTextColor(getColor(R.color.muted))
            txtUsbWarn.visibility = View.GONE
        }
    }

    private fun refreshStatus() {
        val running = WebcamService.running
        btnService.setText(if (running) R.string.stop_service else R.string.start_service)
        btnOverlay.visibility = if (Settings.canDrawOverlays(this)) View.GONE else View.VISIBLE
        if (!editName.hasFocus() && editName.text.toString() != prefs.deviceName) editName.setText(prefs.deviceName)
        btnBattery.setText(if (isIgnoringBattery()) R.string.battery_opt_done else R.string.battery_opt)

        val sb = StringBuilder()
        if (!NativeBridge.available) {
            sb.append(getString(R.string.native_missing_detail, NativeBridge.loadError ?: "?")).append('\n')
        }
        if (!running) {
            sb.append(getString(R.string.lbl_state)).append(": ").append(getString(R.string.state_stopped))
            txtStatus.text = sb.toString()
            return
        }
        val raw = NativeBridge.safeStatus()
        if (raw == null) {
            sb.append(getString(R.string.lbl_state)).append(": ").append(getString(R.string.state_idle))
            txtStatus.text = sb.toString()
            return
        }
        try {
            val j = JSONObject(raw)
            val state = when (j.optString("state")) {
                "listening" -> getString(R.string.state_listening)
                "connected" -> getString(R.string.state_connected)
                "streaming" -> getString(R.string.state_streaming)
                else -> getString(R.string.state_idle)
            }
            sb.append(getString(R.string.lbl_state)).append(": ").append(state).append('\n')
            val peer = j.optString("peer", "")
            if (peer.isNotEmpty()) {
                sb.append(getString(R.string.lbl_peer)).append(": ").append(peer).append('\n')
                sb.append(getString(R.string.lbl_transport)).append(": ")
                    .append(j.optString("transport", "-").uppercase(Locale.ROOT)).append('\n')
            }
            val w = j.optInt("width"); val h = j.optInt("height")
            if (w > 0 && h > 0) {
                sb.append(getString(R.string.lbl_video)).append(": ")
                    .append(String.format(Locale.US, "%dx%d @ %.1f fps  %s", w, h, j.optDouble("fps", 0.0),
                        j.optString("codec", "").uppercase(Locale.ROOT))).append('\n')
                sb.append(getString(R.string.lbl_bitrate)).append(": ")
                    .append(j.optInt("bitrateKbps")).append(" kbps\n")
                sb.append(getString(R.string.lbl_latency)).append(": ")
                    .append(String.format(Locale.US, "%.1f ms", j.optDouble("encodeLatencyMs", 0.0))).append('\n')
                sb.append(getString(R.string.lbl_dropped)).append(": ").append(j.optInt("dropped"))
            }
        } catch (t: Throwable) {
            sb.append(raw)
        }
        txtStatus.text = sb.toString().trimEnd()
    }
}
