package com.prototracer.remote

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.GridLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.ColorRes
import androidx.annotation.IdRes
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.content.ContextCompat
import androidx.core.os.LocaleListCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.slider.Slider
import com.google.android.material.switchmaterial.SwitchMaterial
import com.google.gson.JsonObject
import com.prototracer.remote.ble.BleManager
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.roundToInt

class MainActivity : AppCompatActivity() {

    private data class ExpressionItem(val name: String, val index: Int)

    private lateinit var ble: BleManager
    private val deviceButtons = linkedMapOf<String, MaterialButton>()
    private val expressionButtons = mutableListOf<MaterialButton>()
    private val huePresetViews = mutableListOf<Pair<Int, View>>()
    private val logEntries = ArrayList<String>()

    private var connected = false
    private var currentDeviceName = "ProtoTracer"
    private var currentExpression = 0
    private var brightness = 105
    private var hue = 0
    private var voiceEnabled = true
    private var displayMode = 0
    private var suppressControlCallbacks = false
    private var pendingScanFilter: String? = null
    private var pendingAutoConnect = false
    private var autoConnectFilter: String? = null
    private var connectFlowActive = false

    private val expressions = mutableListOf<ExpressionItem>()

    private val btEnableLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (ble.isBtEnabled()) {
            startScan(pendingScanFilter, pendingAutoConnect)
        } else {
            Toast.makeText(this, R.string.err_bluetooth_disabled, Toast.LENGTH_LONG).show()
        }
    }

    private val permLauncher = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { perms ->
        if (perms.values.all { it }) {
            startScan(pendingScanFilter, pendingAutoConnect)
        } else {
            Toast.makeText(this, R.string.permission_bluetooth_rationale, Toast.LENGTH_LONG).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        ble = BleManager(this).apply {
            onDeviceFound = { name, address, rssi -> runOnUiThread { onDeviceFound(name, address, rssi) } }
            onConnected = { name, address -> runOnUiThread { onBleConnected(name, address) } }
            onDisconnected = { unexpected -> runOnUiThread { onBleDisconnected(unexpected) } }
            onMessage = { message, raw -> runOnUiThread { onBleMessage(message, raw) } }
            onCommandSent = { op, payload ->
                runOnUiThread {
                    addLog("data", getString(R.string.log_sent, op, payload.toByteArray().size))
                }
            }
            onError = { message -> runOnUiThread { onBleError(message) } }
        }

        bindUi()
        updateLanguageButton()
        updateBleSupport()
        setStatus("disconnected", getString(R.string.status_disconnected))
        renderExpressionGrid()
        addLog("info", getString(R.string.log_ready))
    }

    private fun bindUi() {
        view<MaterialButton>(R.id.btn_lang_toggle).setOnClickListener { toggleLanguage() }
        view<MaterialButton>(R.id.btn_scan).setOnClickListener { checkPermsAndScan(null, false) }
        view<MaterialButton>(R.id.btn_stop_scan).setOnClickListener {
            ble.stopScan()
            setScanning(false)
        }
        view<MaterialButton>(R.id.btn_manual_connect).setOnClickListener { onManualConnectClick() }
        view<EditText>(R.id.input_manual_device).setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                onManualConnectClick()
                true
            } else {
                false
            }
        }

        val brightnessSlider = view<Slider>(R.id.slider_brightness)
        brightnessSlider.addOnChangeListener { _, value, _ ->
            brightness = value.roundToInt()
            view<TextView>(R.id.tv_brightness_value).text = brightness.toString()
        }
        brightnessSlider.addOnSliderTouchListener(object : Slider.OnSliderTouchListener {
            override fun onStartTrackingTouch(slider: Slider) = Unit
            override fun onStopTrackingTouch(slider: Slider) {
                if (!suppressControlCallbacks) sendControlInt("brightness", slider.value.roundToInt())
            }
        })

        val hueSlider = view<Slider>(R.id.slider_hue)
        hueSlider.addOnChangeListener { _, value, _ ->
            hue = value.roundToInt()
            view<TextView>(R.id.tv_hue_value).text = "$hue°"
            updateHuePresets()
        }
        hueSlider.addOnSliderTouchListener(object : Slider.OnSliderTouchListener {
            override fun onStartTrackingTouch(slider: Slider) = Unit
            override fun onStopTrackingTouch(slider: Slider) {
                if (!suppressControlCallbacks) sendControlFloat("hue_shift", slider.value)
            }
        })

        view<SwitchMaterial>(R.id.switch_voice).setOnCheckedChangeListener { _, checked ->
            if (suppressControlCallbacks) return@setOnCheckedChangeListener
            voiceEnabled = checked
            sendControlBool("voice_enabled", checked)
            addLog("info", getString(if (checked) R.string.log_voice_on else R.string.log_voice_off))
        }
        view<SwitchMaterial>(R.id.switch_display_mode).setOnCheckedChangeListener { _, checked ->
            if (suppressControlCallbacks) return@setOnCheckedChangeListener
            displayMode = if (checked) 1 else 0
            sendControlInt("display_mode", displayMode)
            addLog("info", getString(if (checked) R.string.log_disp_on else R.string.log_disp_off))
        }

        view<MaterialButton>(R.id.btn_ping).setOnClickListener { sendPing() }
        view<MaterialButton>(R.id.btn_refresh_manifest).setOnClickListener {
            requestManifest()
            addLog("info", getString(R.string.log_manifest_refresh))
        }
        view<MaterialButton>(R.id.btn_disconnect).setOnClickListener {
            try {
                ble.disconnect()
            } catch (e: Exception) {
                addLog("error", getString(R.string.err_disconnect, e.message ?: "Unknown"))
            }
        }
        view<MaterialButton>(R.id.btn_clear_log).setOnClickListener {
            logEntries.clear()
            view<TextView>(R.id.tv_log_output).text = ""
        }

        createHuePresets()
    }

    private fun updateBleSupport() {
        val supportView = view<TextView>(R.id.tv_ble_support)
        val supported = ble.hasAdapter()
        supportView.text = getString(if (supported) R.string.ble_supported else R.string.ble_unsupported)
        supportView.setTextColor(color(if (supported) R.color.success else R.color.danger))
        view<MaterialButton>(R.id.btn_scan).isEnabled = supported
        view<MaterialButton>(R.id.btn_manual_connect).isEnabled = supported
    }

    private fun onManualConnectClick() {
        val query = view<EditText>(R.id.input_manual_device).text.toString().trim()
        if (query.isBlank()) {
            Toast.makeText(this, R.string.err_no_device_name, Toast.LENGTH_SHORT).show()
            return
        }

        if (isMacAddress(query)) {
            connectToDevice(query, query)
        } else {
            checkPermsAndScan(query, true)
        }
    }

    private fun checkPermsAndScan(filter: String?, autoConnect: Boolean) {
        pendingScanFilter = filter
        pendingAutoConnect = autoConnect

        if (!ble.hasAdapter()) {
            Toast.makeText(this, R.string.err_ble_not_supported, Toast.LENGTH_LONG).show()
            return
        }
        if (!ble.isBtEnabled()) {
            btEnableLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            return
        }

        val permissions = requiredBlePermissions().filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (permissions.isNotEmpty()) {
            permLauncher.launch(permissions.toTypedArray())
        } else {
            startScan(filter, autoConnect)
        }
    }

    private fun startScan(filter: String?, autoConnect: Boolean) {
        pendingScanFilter = filter
        pendingAutoConnect = autoConnect
        autoConnectFilter = if (autoConnect) filter?.lowercase(Locale.US) else null
        connectFlowActive = false
        deviceButtons.clear()
        view<LinearLayout>(R.id.layout_devices).removeAllViews()
        view<TextView>(R.id.tv_no_devices).visibility = View.VISIBLE
        setScanning(true)
        ble.startScan(filter)
    }

    private fun setScanning(scanning: Boolean) {
        view<MaterialButton>(R.id.btn_scan).isEnabled = !scanning && ble.hasAdapter()
        view<MaterialButton>(R.id.btn_stop_scan).isEnabled = scanning
        view<TextView>(R.id.tv_scan_status).text = if (scanning) getString(R.string.connect_scan_hint) else ""
        if (scanning) {
            setStatus("scanning", getString(R.string.status_scanning))
        } else if (!connected) {
            setStatus("disconnected", getString(R.string.status_disconnected))
        }
    }

    private fun onDeviceFound(name: String, address: String, rssi: Int) {
        val query = autoConnectFilter
        if (!query.isNullOrBlank() && !connectFlowActive) {
            val match = name.lowercase(Locale.US).contains(query) || address.lowercase(Locale.US).contains(query)
            if (match) {
                connectToDevice(name, address)
                return
            }
        }

        view<TextView>(R.id.tv_no_devices).visibility = View.GONE
        val layout = view<LinearLayout>(R.id.layout_devices)
        val existing = deviceButtons[address]
        val label = "$name\n$address  RSSI $rssi"
        if (existing != null) {
            existing.text = label
            return
        }

        val button = MaterialButton(this).apply {
            text = label
            textSize = 12f
            isAllCaps = false
            gravity = Gravity.CENTER_VERTICAL or Gravity.START
            maxLines = 2
            setTextColor(color(R.color.text_primary))
            backgroundTintList = ColorStateList.valueOf(color(R.color.bg_secondary))
            strokeColor = ColorStateList.valueOf(color(R.color.border))
            strokeWidth = dp(1)
            cornerRadius = dp(8)
            setOnClickListener { connectToDevice(name, address) }
        }
        val params = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = dp(6) }
        deviceButtons[address] = button
        layout.addView(button, params)
    }

    private fun connectToDevice(name: String, address: String) {
        connectFlowActive = true
        currentDeviceName = name
        ble.stopScan()
        setScanning(false)
        setStatus("connecting", getString(R.string.ble_connecting, name))
        addLog("info", getString(R.string.log_found, name))
        ble.connect(address, name)
    }

    private fun onBleConnected(name: String, address: String) {
        connected = true
        connectFlowActive = false
        currentDeviceName = name
        view<TextView>(R.id.tv_device_info).apply {
            text = "$name  $address"
            visibility = View.VISIBLE
        }
        view<LinearLayout>(R.id.connect_panel).visibility = View.GONE
        view<LinearLayout>(R.id.control_panel).visibility = View.VISIBLE
        setStatus("connected", getString(R.string.ble_connected, name))
        addLog("success", getString(R.string.log_connected, name))
        requestManifest()
    }

    private fun onBleDisconnected(unexpected: Boolean) {
        connected = false
        connectFlowActive = false
        autoConnectFilter = null
        deviceButtons.clear()
        view<LinearLayout>(R.id.layout_devices).removeAllViews()
        view<TextView>(R.id.tv_device_info).visibility = View.GONE
        view<LinearLayout>(R.id.control_panel).visibility = View.GONE
        view<LinearLayout>(R.id.connect_panel).visibility = View.VISIBLE
        setStatus("disconnected", getString(R.string.status_disconnected))
        addLog("warn", getString(if (unexpected) R.string.log_disconnected_unexpected else R.string.log_disconnected))
    }

    private fun onBleError(message: String) {
        setStatus("error", message)
        addLog("error", message)
        Toast.makeText(this, message, Toast.LENGTH_LONG).show()
    }

    private fun requestManifest() {
        if (!connected) return
        setStatus("loading", getString(R.string.ble_manifest_loading))
        ble.send("{\"op\":\"config.get\"}")
    }

    private fun sendPing() {
        if (!connected) return
        ble.send("{\"op\":\"ping\"}")
        addLog("info", getString(R.string.log_ping_sent))
    }

    private fun onBleMessage(message: JsonObject, raw: String) {
        when (message.string("op")) {
            "control.state" -> applyControlState(message)
            "pong" -> {
                val name = message.string("name") ?: currentDeviceName
                addLog("success", getString(R.string.log_pong, name))
                Toast.makeText(this, getString(R.string.log_pong, name), Toast.LENGTH_SHORT).show()
            }
            else -> {
                if (message.has("visual") || message.has("pairing") || message.has("device")) {
                    applyManifest(message)
                    setStatus("ready", getString(R.string.ble_manifest_loaded))
                } else {
                    val preview = raw.take(120) + if (raw.length > 120) "..." else ""
                    addLog("data", getString(R.string.log_recv, preview))
                }
            }
        }
    }

    private fun applyManifest(message: JsonObject) {
        addLog("success", getString(R.string.log_manifest_received))
        val visual = message.obj("visual")
        val device = message.obj("device")

        val names = visual?.getAsJsonArray("expression_names")
        expressions.clear()
        if (names != null && names.size() > 0) {
            names.forEachIndexed { index, element ->
                expressions.add(ExpressionItem(element.asString, index))
            }
        } else {
            val count = visual?.int("expression_count") ?: 0
            for (i in 0 until count) {
                expressions.add(ExpressionItem(getString(R.string.log_expr_fallback, i + 1), i))
            }
        }

        device?.string("display_name")?.takeIf { it.isNotBlank() }?.let {
            currentDeviceName = it
            view<TextView>(R.id.tv_device_info).text = it
        }

        view<TextView>(R.id.tv_expr_count).text = getString(R.string.ctrl_expr_count, expressions.size)
        renderExpressionGrid()
        val animationName = visual?.string("animation_name") ?: "Unknown"
        addLog("info", getString(R.string.log_animation, animationName, expressions.size))
    }

    private fun applyControlState(message: JsonObject) {
        suppressControlCallbacks = true
        message.int("expression")?.let {
            currentExpression = it
            highlightExpression(it)
        }
        message.int("brightness")?.let {
            brightness = it.coerceIn(0, 255)
            view<Slider>(R.id.slider_brightness).value = brightness.toFloat()
            view<TextView>(R.id.tv_brightness_value).text = brightness.toString()
        }
        message.bool("voice_enabled")?.let {
            voiceEnabled = it
            view<SwitchMaterial>(R.id.switch_voice).isChecked = it
        }
        message.int("display_mode")?.let {
            displayMode = it
            view<SwitchMaterial>(R.id.switch_display_mode).isChecked = it != 0
        }
        message.float("hue_shift")?.let {
            hue = it.roundToInt().coerceIn(0, 360)
            view<Slider>(R.id.slider_hue).value = hue.toFloat()
            view<TextView>(R.id.tv_hue_value).text = "$hue°"
            updateHuePresets()
        }
        suppressControlCallbacks = false
    }

    private fun renderExpressionGrid() {
        val grid = view<GridLayout>(R.id.expression_grid)
        grid.removeAllViews()
        expressionButtons.clear()
        view<TextView>(R.id.tv_expr_count).text = getString(R.string.ctrl_expr_count, expressions.size)

        val columnCount = if (resources.configuration.screenWidthDp >= 600) 3 else 2
        grid.columnCount = columnCount
        expressions.forEach { expression ->
            val button = MaterialButton(this).apply {
                text = expression.name
                isAllCaps = false
                textSize = 12f
                minHeight = dp(44)
                maxLines = 2
                gravity = Gravity.CENTER
                cornerRadius = dp(8)
                strokeWidth = dp(1)
                setOnClickListener { selectExpression(expression.index) }
            }
            val params = GridLayout.LayoutParams().apply {
                width = 0
                height = ViewGroup.LayoutParams.WRAP_CONTENT
                columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1, 1f)
                setMargins(dp(4), dp(4), dp(4), dp(4))
            }
            expressionButtons.add(button)
            grid.addView(button, params)
        }
        highlightExpression(currentExpression)
    }

    private fun selectExpression(index: Int) {
        currentExpression = index
        highlightExpression(index)
        val name = expressions.firstOrNull { it.index == index }?.name ?: index.toString()
        sendControlInt("expression", index)
        addLog("info", getString(R.string.log_expr_set, name))
    }

    private fun highlightExpression(index: Int) {
        expressions.forEachIndexed { buttonIndex, expression ->
            val button = expressionButtons.getOrNull(buttonIndex) ?: return@forEachIndexed
            val active = expression.index == index
            button.backgroundTintList = ColorStateList.valueOf(color(if (active) R.color.accent else R.color.bg_secondary))
            button.strokeColor = ColorStateList.valueOf(color(if (active) R.color.accent else R.color.border))
            button.setTextColor(color(if (active) R.color.on_primary else R.color.text_primary))
        }
    }

    private fun sendControlInt(field: String, value: Int) {
        sendControl(JsonObject().apply { addProperty(field, value) })
    }

    private fun sendControlFloat(field: String, value: Float) {
        sendControl(JsonObject().apply { addProperty(field, value) })
    }

    private fun sendControlBool(field: String, value: Boolean) {
        sendControl(JsonObject().apply { addProperty(field, value) })
    }

    private fun sendControl(fields: JsonObject) {
        if (!connected) {
            Toast.makeText(this, R.string.err_not_connected, Toast.LENGTH_SHORT).show()
            return
        }
        val payload = JsonObject().apply {
            addProperty("op", "control.set")
            fields.entrySet().forEach { (key, value) -> add(key, value) }
        }
        ble.send(payload.toString())
    }

    private fun createHuePresets() {
        val presets = listOf(
            0 to Color.rgb(255, 68, 68),
            30 to Color.rgb(255, 136, 68),
            60 to Color.rgb(255, 221, 68),
            120 to Color.rgb(68, 255, 68),
            180 to Color.rgb(68, 221, 255),
            240 to Color.rgb(68, 68, 255),
            280 to Color.rgb(170, 68, 255),
            320 to Color.rgb(255, 68, 170)
        )
        val container = view<LinearLayout>(R.id.hue_presets)
        container.removeAllViews()
        huePresetViews.clear()
        presets.forEach { (degrees, colorValue) ->
            val swatch = View(this).apply {
                background = circleDrawable(colorValue, false)
                setOnClickListener {
                    view<Slider>(R.id.slider_hue).value = degrees.toFloat()
                    sendControlFloat("hue_shift", degrees.toFloat())
                }
            }
            val params = LinearLayout.LayoutParams(dp(28), dp(28)).apply {
                marginStart = dp(4)
                marginEnd = dp(4)
            }
            huePresetViews.add(degrees to swatch)
            container.addView(swatch, params)
        }
        updateHuePresets()
    }

    private fun updateHuePresets() {
        huePresetViews.forEach { (degrees, swatch) ->
            val active = degrees == hue
            val colorValue = (swatch.background as? GradientDrawable)?.color?.defaultColor ?: Color.TRANSPARENT
            swatch.background = circleDrawable(colorValue, active)
        }
    }

    private fun setStatus(state: String, message: String) {
        view<TextView>(R.id.tv_status).text = message
        val colorRes = when (state) {
            "connected", "ready" -> R.color.success
            "connecting", "scanning", "loading", "found" -> R.color.warning
            "error" -> R.color.danger
            else -> R.color.text_muted
        }
        view<View>(R.id.status_dot).background = circleDrawable(color(colorRes), false)
    }

    private fun addLog(level: String, message: String) {
        val ts = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        logEntries.add("$ts ${level.uppercase(Locale.US)}  $message")
        while (logEntries.size > 200) logEntries.removeAt(0)
        view<TextView>(R.id.tv_log_output).text = logEntries.joinToString("\n")
    }

    private fun toggleLanguage() {
        val next = if (currentLanguage() == "zh") "en" else "zh"
        AppCompatDelegate.setApplicationLocales(LocaleListCompat.forLanguageTags(next))
    }

    private fun updateLanguageButton() {
        view<MaterialButton>(R.id.btn_lang_toggle).text = if (currentLanguage() == "zh") "EN" else "中"
    }

    private fun currentLanguage(): String {
        val appLocale = AppCompatDelegate.getApplicationLocales()[0]
        if (appLocale != null) return appLocale.language
        return resources.configuration.locales[0]?.language ?: Locale.getDefault().language
    }

    private fun requiredBlePermissions(): Array<String> {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }

    private fun isMacAddress(value: String): Boolean {
        return Regex("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$").matches(value)
    }

    private fun circleDrawable(color: Int, active: Boolean): GradientDrawable {
        return GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(color)
            if (active) setStroke(dp(2), Color.WHITE)
        }
    }

    private fun JsonObject.string(name: String): String? = try {
        get(name)?.takeIf { !it.isJsonNull }?.asString
    } catch (_: Exception) {
        null
    }

    private fun JsonObject.int(name: String): Int? = try {
        get(name)?.takeIf { !it.isJsonNull }?.asInt
    } catch (_: Exception) {
        null
    }

    private fun JsonObject.float(name: String): Float? = try {
        get(name)?.takeIf { !it.isJsonNull }?.asFloat
    } catch (_: Exception) {
        null
    }

    private fun JsonObject.bool(name: String): Boolean? = try {
        get(name)?.takeIf { !it.isJsonNull }?.asBoolean
    } catch (_: Exception) {
        null
    }

    private fun JsonObject.obj(name: String): JsonObject? = try {
        getAsJsonObject(name)
    } catch (_: Exception) {
        null
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).roundToInt()

    private fun color(@ColorRes resId: Int): Int = ContextCompat.getColor(this, resId)

    private fun <T : View> view(@IdRes id: Int): T = findViewById(id)

    override fun onDestroy() {
        ble.disconnect()
        super.onDestroy()
    }
}