package com.prototracer.remote

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.*
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.prototracer.remote.ble.BleManager

class MainActivity : AppCompatActivity() {

    // Hardcoded expression names from firmware kRemoteControllerExpressionNames
    private val expressions = listOf(
        "Default", "Angry", "Doubt", "Frown", "Heart", "Sad",
        "Surprise", "Happy", "OwO", "Sleepy", "Curious", "Excited",
        "Wink", "Shy", "Focus", "Custom"
    )

    private lateinit var ble: BleManager
    private var exprIdx = 0
    private var rxUuid: String? = null
    private var connected = false
    private var connectFlowActive = false
    private val deviceButtons = linkedMapOf<String, Button>()
    private var pendingDeviceName: String? = null
    private var pendingDeviceAddr: String? = null

    // QR scanner
    private val qrLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { r ->
        connectFlowActive = false
        if (r.resultCode == RESULT_OK) {
            rxUuid = r.data?.getStringExtra(QrScannerActivity.EXTRA_QR_RESULT)
            if (!rxUuid.isNullOrBlank()) {
                findViewById<TextView>(R.id.tv_rx_uuid).apply {
                    text = "RX UUID: $rxUuid"
                    visibility = View.VISIBLE
                }
                Toast.makeText(this, "RX UUID scanned", Toast.LENGTH_SHORT).show()
                connectPendingDevice()
            }
        } else {
            pendingDeviceName = null
            pendingDeviceAddr = null
            findViewById<TextView>(R.id.tv_status).text = "Disconnected"
        }
    }

    // Bluetooth enable
    private val btEnableLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (it.resultCode == RESULT_OK) doScan()
    }

    // Permissions
    private val permLauncher = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { perms ->
        if (perms.values.all { it }) doScan()
        else Toast.makeText(this, "Need BLE permission", Toast.LENGTH_LONG).show()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        ble = BleManager(this).apply {
            onDeviceFound = { name, addr -> addDeviceButton(name, addr) }
            onConnected = { runOnUiThread { onReady() } }
            onDisconnected = { runOnUiThread { onDisconnected() } }
            onError = { msg -> runOnUiThread { Toast.makeText(this@MainActivity, msg, Toast.LENGTH_LONG).show() } }
        }

        findViewById<Button>(R.id.btn_qr).visibility = View.GONE
        findViewById<Button>(R.id.btn_scan).setOnClickListener { checkPermsAndScan() }
        findViewById<Button>(R.id.btn_stop_scan).setOnClickListener { ble.stopScan(); setScanning(false) }

        // Expression
        findViewById<Button>(R.id.btn_prev).setOnClickListener { exprIdx = (exprIdx - 1 + expressions.size) % expressions.size; showExpr() }
        findViewById<Button>(R.id.btn_next).setOnClickListener { exprIdx = (exprIdx + 1) % expressions.size; showExpr() }
        findViewById<Button>(R.id.btn_send_expr).setOnClickListener { send("expression", exprIdx) }

        // Brightness
        val sBright = findViewById<com.google.android.material.slider.Slider>(R.id.slider_bright)
        sBright.addOnChangeListener { _, v, _ -> findViewById<TextView>(R.id.tv_bright).text = v.toInt().toString() }
        findViewById<Button>(R.id.btn_send_bright).setOnClickListener { send("brightness", sBright.value.toInt()) }

        // Hue
        val sHue = findViewById<com.google.android.material.slider.Slider>(R.id.slider_hue)
        sHue.addOnChangeListener { _, v, _ ->
            findViewById<TextView>(R.id.tv_hue).text = v.toInt().toString()
            findViewById<View>(R.id.view_hue).setBackgroundColor(hsvToColor(v, 1f, 1f))
        }
        findViewById<Button>(R.id.btn_send_hue).setOnClickListener { send("hue_shift", sHue.value) }

        // Toggles
        val swVoice = findViewById<com.google.android.material.switchmaterial.SwitchMaterial>(R.id.switch_voice)
        swVoice.setOnCheckedChangeListener { _, on -> send("voice_enabled", on) }

        val swFan = findViewById<com.google.android.material.switchmaterial.SwitchMaterial>(R.id.switch_fan)
        swFan.setOnCheckedChangeListener { _, on -> send("display_mode", if (on) 1 else 0) }

        findViewById<Button>(R.id.btn_disconnect).setOnClickListener { ble.disconnect(); onDisconnected() }
    }

    private fun checkCameraPerm(onGranted: () -> Unit) {
        onGranted()
    }

    private fun connectPendingDevice() {
        val addr = pendingDeviceAddr ?: return
        val name = pendingDeviceName ?: "device"
        pendingDeviceAddr = null
        pendingDeviceName = null

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(this, "Need Bluetooth connect permission", Toast.LENGTH_LONG).show()
            return
        }

        findViewById<TextView>(R.id.tv_status).text = "Connecting to $name..."
        ble.connect(addr)
    }

    private fun checkPermsAndScan() {
        if (!ble.isBtEnabled()) {
            btEnableLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            return
        }
        val perms = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED)
                perms.add(Manifest.permission.BLUETOOTH_SCAN)
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED)
                perms.add(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED)
                perms.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        if (perms.isNotEmpty()) permLauncher.launch(perms.toTypedArray()) else doScan()
    }

    private fun doScan() {
        setScanning(true)
        deviceButtons.clear()
        findViewById<LinearLayout>(R.id.layout_devices).apply { removeAllViews(); visibility = View.VISIBLE }
        ble.startScan()
    }

    private fun setScanning(on: Boolean) {
        findViewById<Button>(R.id.btn_scan).isEnabled = !on
        findViewById<Button>(R.id.btn_stop_scan).isEnabled = on
        findViewById<TextView>(R.id.tv_no_devices).visibility = View.GONE
        findViewById<TextView>(R.id.tv_status).text = if (on) "Scanning..." else "Disconnected"
    }

    private fun addDeviceButton(name: String, addr: String) {
        runOnUiThread {
            findViewById<TextView>(R.id.tv_no_devices).visibility = View.GONE
            val layout = findViewById<LinearLayout>(R.id.layout_devices)
            val existing = deviceButtons[addr]
            if (existing != null) {
                existing.text = "$name\n$addr"
                return@runOnUiThread
            }

            val btn = Button(this).apply {
                text = "$name\n$addr"
                textSize = 12f
                setOnClickListener {
                    if (connectFlowActive) return@setOnClickListener
                    connectFlowActive = true
                    ble.stopScan()
                    setScanning(false)
                    pendingDeviceName = name
                    pendingDeviceAddr = addr
                    findViewById<TextView>(R.id.tv_status).text = "Scan QR for $name"
                    try {
                        qrLauncher.launch(Intent(this@MainActivity, QrScannerActivity::class.java))
                    } catch (e: Exception) {
                        Toast.makeText(this@MainActivity, "QR screen failed, connecting directly", Toast.LENGTH_LONG).show()
                        connectFlowActive = false
                        connectPendingDevice()
                    }
                }
            }
            deviceButtons[addr] = btn
            layout.addView(btn)
        }
    }

    private fun onReady() {
        connected = true
        findViewById<TextView>(R.id.tv_status).text = "Connected"
        findViewById<LinearLayout>(R.id.layout_devices).visibility = View.GONE
        findViewById<LinearLayout>(R.id.layout_controls).visibility = View.VISIBLE
        showExpr()
    }

    private fun onDisconnected() {
        connected = false
        deviceButtons.clear()
        findViewById<TextView>(R.id.tv_status).text = "Disconnected"
        findViewById<LinearLayout>(R.id.layout_controls).visibility = View.GONE
        findViewById<LinearLayout>(R.id.layout_devices).visibility = View.GONE
        findViewById<TextView>(R.id.tv_no_devices).visibility = View.VISIBLE
    }

    private fun showExpr() {
        findViewById<TextView>(R.id.tv_expr).text = "$exprIdx: ${expressions[exprIdx]}"
    }

    /** Send a single-field control.set JSON following the firmware's ApplyJsonCommand format. */
    private fun send(field: String, value: Any) {
        if (!connected) return
        val v = when (value) {
            is Boolean -> if (value) 1 else 0
            is Int -> value
            is Float -> value
            else -> return
        }
        // Format fields as firmware expects: expression→int, brightness→int, hue_shift→float, voice_enabled→bool, display_mode→int
        val json = buildString {
            append("""{"op":"control.set",""")
            when (field) {
                "expression" -> append(""""expression":$v""")
                "brightness" -> append(""""brightness":$v""")
                "hue_shift" -> append(""""hue_shift":${value as Float}""")
                "voice_enabled" -> append(""""voice_enabled":${value == 1}""")
                "display_mode" -> append(""""display_mode":$v""")
            }
            append("}")
        }
        ble.send(json)
        Toast.makeText(this, "Sent: $field=$value", Toast.LENGTH_SHORT).show()
    }

    private fun hsvToColor(h: Float, s: Float, v: Float): Int {
        val c = v * s
        val x = c * (1 - Math.abs((h / 60) % 2 - 1))
        val m = v - c
        val (r, g, b) = when ((h / 60).toInt()) {
            0 -> Triple(c, x, 0f); 1 -> Triple(x, c, 0f); 2 -> Triple(0f, c, x)
            3 -> Triple(0f, x, c); 4 -> Triple(x, 0f, c); 5 -> Triple(c, 0f, x)
            else -> Triple(0f, 0f, 0f)
        }
        return Color.rgb(((r + m) * 255).toInt().coerceIn(0, 255), ((g + m) * 255).toInt().coerceIn(0, 255), ((b + m) * 255).toInt().coerceIn(0, 255))
    }

    override fun onDestroy() { super.onDestroy(); ble.disconnect() }
}
