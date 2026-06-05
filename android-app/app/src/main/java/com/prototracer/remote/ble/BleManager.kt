package com.prototracer.remote.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.prototracer.remote.model.BleConstants
import no.nordicsemi.android.ble.BleManager as NordicBleManager
import no.nordicsemi.android.ble.callback.FailCallback
import no.nordicsemi.android.ble.data.DataSplitter
import no.nordicsemi.android.ble.observer.ConnectionObserver
import java.nio.charset.StandardCharsets
import java.util.Locale
import java.util.UUID

@SuppressLint("MissingPermission")
class BleManager(private val ctx: Context) {

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString(BleConstants.SERVICE_UUID)
        private const val TAG = "ProtoTracerBle"
        private const val BLE_CHUNK_BYTES = 160
        private const val DESIRED_MTU = 247
        private const val CONNECT_TIMEOUT_MS = 20000L
        private const val CONNECT_RETRY_DELAY_MS = 1500
        private const val MAX_CONNECT_ATTEMPTS = 2
        private const val SCAN_TO_CONNECT_SETTLE_MS = 500L
    }

    private val adapter: BluetoothAdapter? =
        (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    private var scanner = adapter?.bluetoothLeScanner
    private var connected = false
    private var intentionalDisconnect = false
    private var currentDeviceName = "ProtoTracer"
    private var currentDeviceAddress = ""
    private var scanNameFilter: String? = null
    private val txBuffer = StringBuilder()

    var onDeviceFound: ((String, String, Int) -> Unit)? = null
    var onConnected: ((String, String) -> Unit)? = null
    var onDisconnected: ((Boolean) -> Unit)? = null
    var onMessage: ((JsonObject, String) -> Unit)? = null
    var onCommandSent: ((String, String) -> Unit)? = null
    var onError: ((String) -> Unit)? = null

    private val mainHandler = Handler(Looper.getMainLooper())
    private val nordicManager = ProtoTracerNordicManager(ctx.applicationContext)

    private val scanCb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            try {
                val scanResult = result ?: return
                val device = scanResult.device ?: return
                val address = device.address ?: return
                val scanName = scanResult.scanRecord?.deviceName
                val name = if (!scanName.isNullOrBlank()) scanName else safeDeviceName(device) ?: "ProtoTracer"
                val hasProtoService =
                    scanResult.scanRecord?.serviceUuids?.contains(ParcelUuid(SERVICE_UUID)) == true
                if (!hasProtoService) return

                val filter = scanNameFilter?.lowercase(Locale.US)?.trim().orEmpty()
                if (filter.isNotEmpty() &&
                    !name.lowercase(Locale.US).contains(filter) &&
                    !address.lowercase(Locale.US).contains(filter)
                ) {
                    return
                }

                Log.d(TAG, "Scan found: $name ($address) rssi=${scanResult.rssi} hasProtoService=$hasProtoService")
                onDeviceFound?.invoke(name, address, scanResult.rssi)
            } catch (se: SecurityException) {
                Log.e(TAG, "Scan result security error", se)
                onError?.invoke("Bluetooth permission missing while scanning")
            } catch (e: Exception) {
                Log.e(TAG, "Scan parse error", e)
                onError?.invoke("Scan parse error: ${e.message}")
            }
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "Scan failed with error code $errorCode")
            onError?.invoke("Scan failed: $errorCode")
        }
    }

    fun hasAdapter(): Boolean = adapter != null

    fun isBtEnabled(): Boolean = adapter?.isEnabled == true

    fun isConnected(): Boolean = connected

    fun startScan(nameFilter: String? = null) {
        Log.d(TAG, "=== startScan === filter='${nameFilter ?: "none"}' sdk=${Build.VERSION.SDK_INT}")
        try {
            scanner = adapter?.bluetoothLeScanner
            stopScan()
            scanNameFilter = nameFilter
            val filters = listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build())
            val settings = ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build()
            Log.d(TAG, "Scan starting with service filter mode=LOW_LATENCY")
            scanner?.startScan(filters, settings, scanCb)
        } catch (se: SecurityException) {
            Log.e(TAG, "Scan permission missing", se)
            onError?.invoke("Bluetooth scan permission missing")
        } catch (e: Exception) {
            Log.e(TAG, "Start scan failed", e)
            onError?.invoke("Start scan failed: ${e.message}")
        }
    }

    fun stopScan() {
        Log.d(TAG, "=== stopScan ===")
        try {
            scanner?.stopScan(scanCb)
        } catch (_: Exception) {
        }
        scanNameFilter = null
    }

    fun connect(address: String, name: String = "ProtoTracer") {
        Log.d(TAG, "=== connect === address=$address name='$name' sdk=${Build.VERSION.SDK_INT} autoConnect=false settleMs=$SCAN_TO_CONNECT_SETTLE_MS")
        try {
            stopScan()
            connected = false
            intentionalDisconnect = false
            txBuffer.clear()
            currentDeviceName = name.ifBlank { "ProtoTracer" }
            currentDeviceAddress = address

            val device = adapter?.getRemoteDevice(address) ?: run {
                Log.e(TAG, "No Bluetooth adapter")
                onError?.invoke("No Bluetooth adapter")
                return
            }
            Log.d(TAG, "Device obtained: type=${device.type} bondState=${device.bondState}")

            nordicManager.close()
            Log.d(TAG, "Posting connect with ${SCAN_TO_CONNECT_SETTLE_MS}ms settle delay")
            // Budget Android 9 devices (Qin 2 Pro, etc.) need the BLE radio to
            // settle after scanning before a connection can succeed.
            mainHandler.postDelayed({
                Log.d(TAG, "Settle delay elapsed, starting connectTo")
                nordicManager.connectTo(device)
            }, SCAN_TO_CONNECT_SETTLE_MS)
        } catch (se: SecurityException) {
            Log.e(TAG, "Connect permission missing", se)
            onError?.invoke("Bluetooth connect permission missing")
        } catch (e: Exception) {
            Log.e(TAG, "Connect failed", e)
            onError?.invoke("Connect failed: ${e.message}")
        }
    }

    fun disconnect() {
        Log.d(TAG, "=== disconnect === intentional=true wasConnected=$connected")
        intentionalDisconnect = true
        connected = false
        txBuffer.clear()
        nordicManager.disconnectDevice()
    }

    fun send(json: String) {
        val op = extractOp(json)
        Log.d(TAG, "=== send === op=$op connected=$connected size=${json.length}")
        if (!connected) {
            Log.w(TAG, "Send blocked: not connected")
            onError?.invoke("Not connected to device")
            return
        }
        nordicManager.sendJson(json)
    }

    private inner class ProtoTracerNordicManager(context: Context) : NordicBleManager(context) {

        private var rxChar: BluetoothGattCharacteristic? = null
        private var txChar: BluetoothGattCharacteristic? = null
        private var writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        private val chunkSplitter = DataSplitter { message, index, maxLength ->
            val chunkSize = minOf(BLE_CHUNK_BYTES, maxLength)
            val offset = index * chunkSize
            if (offset >= message.size) {
                null
            } else {
                message.copyOfRange(offset, minOf(offset + chunkSize, message.size))
            }
        }

        init {
            setConnectionObserver(object : ConnectionObserver {
                override fun onDeviceConnecting(device: BluetoothDevice) {
                    Log.d(TAG, "[Nordic] onDeviceConnecting: ${device.address}")
                }

                override fun onDeviceConnected(device: BluetoothDevice) {
                    Log.d(TAG, "[Nordic] onDeviceConnected: ${device.address} name='${safeDeviceName(device)}'")
                    currentDeviceAddress = device.address
                    currentDeviceName = resolveDeviceName(device)
                }

                override fun onDeviceFailedToConnect(device: BluetoothDevice, reason: Int) {
                    connected = false
                    txBuffer.clear()
                    currentDeviceAddress = device.address
                    Log.w(TAG, "[Nordic] onDeviceFailedToConnect: ${device.address} reason=$reason (${describeConnectionReason(reason)})")
                }

                override fun onDeviceReady(device: BluetoothDevice) {
                    Log.d(TAG, "[Nordic] onDeviceReady: ${device.address} — GATT ready, service discovery starting")
                }

                override fun onDeviceDisconnecting(device: BluetoothDevice) {
                    Log.d(TAG, "[Nordic] onDeviceDisconnecting: ${device.address}")
                }

                override fun onDeviceDisconnected(device: BluetoothDevice, reason: Int) {
                    val wasConnected = connected
                    Log.d(TAG, "[Nordic] onDeviceDisconnected: ${device.address} reason=$reason (${describeConnectionReason(reason)}) wasConnected=$wasConnected intentional=$intentionalDisconnect")
                    connected = false
                    txBuffer.clear()
                    val unexpected = wasConnected &&
                        !intentionalDisconnect &&
                        reason != ConnectionObserver.REASON_SUCCESS &&
                        reason != ConnectionObserver.REASON_CANCELLED &&
                        reason != ConnectionObserver.REASON_TERMINATE_LOCAL_HOST
                    intentionalDisconnect = false
                    if (wasConnected) {
                        onDisconnected?.invoke(unexpected)
                    }
                }
            })
        }

        @Suppress("DEPRECATION")
        override fun getGattCallback(): NordicBleManager.BleManagerGattCallback {
            return object : NordicBleManager.BleManagerGattCallback() {
                @Suppress("OVERRIDE_DEPRECATION")
                override fun isRequiredServiceSupported(gatt: BluetoothGatt): Boolean {
                    Log.d(TAG, "[Nordic] isRequiredServiceSupported: searching for service $SERVICE_UUID")
                    val service = gatt.getService(SERVICE_UUID)
                    if (service == null) {
                        Log.w(TAG, "[Nordic] Service NOT found. Available services:")
                        gatt.services?.forEach { s ->
                            Log.w(TAG, "  - ${s.uuid}")
                        }
                        return false
                    }
                    Log.d(TAG, "[Nordic] Service found, ${service.characteristics.size} characteristics:")
                    service.characteristics.forEach { c ->
                        val props = mutableListOf<String>()
                        if ((c.properties and BluetoothGattCharacteristic.PROPERTY_READ) != 0) props.add("READ")
                        if ((c.properties and BluetoothGattCharacteristic.PROPERTY_WRITE) != 0) props.add("WRITE")
                        if ((c.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0) props.add("WRITE_NR")
                        if ((c.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY) != 0) props.add("NOTIFY")
                        Log.d(TAG, "  - ${c.uuid} props=[${props.joinToString(",")}]")
                    }

                    rxChar = service.characteristics.firstOrNull { characteristic ->
                        (characteristic.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0
                    } ?: service.characteristics.firstOrNull { characteristic ->
                        (characteristic.properties and BluetoothGattCharacteristic.PROPERTY_WRITE) != 0
                    }

                    txChar = service.characteristics.firstOrNull { characteristic ->
                        (characteristic.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY) != 0
                    }

                    writeType = rxChar?.let(::preferredWriteType)
                        ?: BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT

                    Log.d(TAG, "[Nordic] Chars resolved: rx=${rxChar?.uuid} tx=${txChar?.uuid} writeType=${describeWriteType(writeType)}")
                    val ok = rxChar != null && txChar != null
                    if (!ok) Log.e(TAG, "[Nordic] Required characteristics missing!")
                    return ok
                }

                @Suppress("OVERRIDE_DEPRECATION")
                override fun initialize() {
                    Log.d(TAG, "[Nordic] initialize: sdk=${Build.VERSION.SDK_INT} requestMtu=${shouldRequestMtu()} desiredMtu=$DESIRED_MTU")
                    setNotificationCallback(txChar).with { _, data ->
                        val bytes = data.value ?: return@with
                        Log.v(TAG, "[Nordic] Notif rx: ${bytes.size}b firstByte=${bytes.firstOrNull()?.toInt()?.and(0xFF)}")
                        handleIncoming(bytes)
                    }

                    if (shouldRequestMtu()) {
                        Log.d(TAG, "[Nordic] Requesting MTU $DESIRED_MTU...")
                        requestMtu(DESIRED_MTU)
                            .done { mtu ->
                                Log.d(TAG, "[Nordic] MTU negotiated: $mtu")
                                enableTxNotifications()
                            }
                            .fail { _, status ->
                                Log.w(
                                    TAG,
                                    "[Nordic] MTU request failed: ${describeRequestStatus(status)}; enabling notifications with default MTU"
                                )
                                enableTxNotifications()
                            }
                            .enqueue()
                    } else {
                        Log.d(TAG, "[Nordic] Skipping MTU request (pre-Q), enabling notifications directly")
                        enableTxNotifications()
                    }
                }

                @Suppress("OVERRIDE_DEPRECATION")
                override fun onServicesInvalidated() {
                    Log.w(TAG, "[Nordic] onServicesInvalidated — clearing characteristic refs")
                    rxChar = null
                    txChar = null
                    writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    txBuffer.clear()
                }
            }
        }

        fun connectTo(device: BluetoothDevice) {
            Log.d(TAG, "[Nordic] connectTo: addr=${device.address} autoConnect=false timeoutMs=$CONNECT_TIMEOUT_MS retries=${MAX_CONNECT_ATTEMPTS-1}")
            // Simple Nordic connect — matches web-app: autoConnect=false, no
            // transport flags, no MTU pre-negotiation.  Nordic handles service
            // discovery → isRequiredServiceSupported → initialize.
            connect(device)
                .useAutoConnect(false)
                .retry(MAX_CONNECT_ATTEMPTS - 1, CONNECT_RETRY_DELAY_MS)
                .timeout(CONNECT_TIMEOUT_MS)
                .done { connectedDevice ->
                    Log.d(TAG, "[Nordic] connect SUCCESS: ${connectedDevice.address}")
                    connected = true
                    intentionalDisconnect = false
                    currentDeviceAddress = connectedDevice.address
                    currentDeviceName = resolveDeviceName(connectedDevice)
                    onConnected?.invoke(currentDeviceName, currentDeviceAddress)
                }
                .fail { failedDevice, status ->
                    Log.e(TAG, "[Nordic] connect FAILED: ${failedDevice.address} status=$status")
                    connected = false
                    txBuffer.clear()
                    if (!intentionalDisconnect) {
                        onError?.invoke(formatConnectError(status))
                    }
                    intentionalDisconnect = false
                }
                .enqueue()
        }

        fun disconnectDevice() {
            Log.d(TAG, "[Nordic] disconnectDevice")
            try {
                disconnect().enqueue()
            } catch (_: Exception) {
                Log.w(TAG, "[Nordic] disconnect enqueue failed, calling close()")
                close()
            }
        }

        fun sendJson(json: String) {
            val characteristic = rxChar
            val bytes = json.toByteArray(StandardCharsets.UTF_8)
            val chunks = ((bytes.size + BLE_CHUNK_BYTES - 1) / BLE_CHUNK_BYTES)
            Log.d(TAG, "[Nordic] sendJson: ${bytes.size}B in $chunks chunks writeType=${describeWriteType(writeType)} char=${characteristic?.uuid}")
            if (!connected || characteristic == null) {
                Log.w(TAG, "[Nordic] sendJson blocked: connected=$connected char=${characteristic?.uuid}")
                onError?.invoke("Not connected to device")
                return
            }

            writeCharacteristic(characteristic, bytes, writeType)
                .split(chunkSplitter)
                .done {
                    Log.d(TAG, "[Nordic] Write OK: ${json.take(40)}")
                    onCommandSent?.invoke(extractOp(json), json)
                }
                .fail { _, status ->
                    Log.w(TAG, "[Nordic] Write FAILED: ${describeRequestStatus(status)}")
                    onError?.invoke("BLE write failed (${describeRequestStatus(status)})")
                }
                .enqueue()
        }

        private fun enableTxNotifications() {
            Log.d(TAG, "[Nordic] enableTxNotifications on ${txChar?.uuid}")
            enableNotifications(txChar)
                .done { Log.d(TAG, "[Nordic] Notifications ENABLED on ${txChar?.uuid}") }
                .fail { _, status ->
                    Log.w(TAG, "[Nordic] Notifications FAILED: ${describeRequestStatus(status)}")
                    onError?.invoke("Notification setup failed (${describeRequestStatus(status)})")
                }
                .enqueue()
        }
    }

    private fun preferredWriteType(characteristic: BluetoothGattCharacteristic): Int {
        return if ((characteristic.properties and BluetoothGattCharacteristic.PROPERTY_WRITE) != 0) {
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        } else if ((characteristic.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0) {
            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        } else {
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        }
    }

    private fun shouldRequestMtu(): Boolean = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q

    private fun describeRequestStatus(status: Int): String {
        return when (status) {
            FailCallback.REASON_DEVICE_DISCONNECTED -> "device disconnected (-1)"
            FailCallback.REASON_DEVICE_NOT_SUPPORTED -> "device not supported (-2)"
            FailCallback.REASON_NULL_ATTRIBUTE -> "missing characteristic (-3)"
            FailCallback.REASON_REQUEST_FAILED -> "request failed (-4)"
            FailCallback.REASON_TIMEOUT -> "timeout (-5)"
            FailCallback.REASON_VALIDATION -> "validation failed (-6)"
            FailCallback.REASON_CANCELLED -> "cancelled (-7)"
            FailCallback.REASON_NOT_ENABLED -> "not enabled (-8)"
            FailCallback.REASON_BLUETOOTH_DISABLED -> "bluetooth disabled (-100)"
            else -> "status $status"
        }
    }

    private fun describeConnectionReason(reason: Int): String {
        return when (reason) {
            ConnectionObserver.REASON_UNKNOWN -> "unknown (-1)"
            ConnectionObserver.REASON_SUCCESS -> "success (0)"
            ConnectionObserver.REASON_TERMINATE_LOCAL_HOST -> "local host (1)"
            ConnectionObserver.REASON_TERMINATE_PEER_USER -> "peer user (2)"
            ConnectionObserver.REASON_LINK_LOSS -> "link loss (3)"
            ConnectionObserver.REASON_NOT_SUPPORTED -> "not supported (4)"
            ConnectionObserver.REASON_CANCELLED -> "cancelled (5)"
            ConnectionObserver.REASON_TIMEOUT -> "timeout (10)"
            else -> "reason $reason"
        }
    }

    private fun describeWriteType(writeType: Int): String {
        return when (writeType) {
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT -> "WRITE (with response)"
            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE -> "WRITE_NR (no response)"
            BluetoothGattCharacteristic.WRITE_TYPE_SIGNED -> "WRITE_SIGNED"
            else -> "type $writeType"
        }
    }

    private fun resolveDeviceName(device: BluetoothDevice): String {
        return currentDeviceName.takeIf { it.isNotBlank() && it != currentDeviceAddress }
            ?: safeDeviceName(device)
            ?: "ProtoTracer"
    }

    private fun formatConnectError(status: Int): String {
        return when (status) {
            ConnectionObserver.REASON_TIMEOUT -> "Connection timed out"
            FailCallback.REASON_TIMEOUT -> "Connection timed out"
            ConnectionObserver.REASON_NOT_SUPPORTED -> "ProtoTracer service not found"
            FailCallback.REASON_DEVICE_NOT_SUPPORTED -> "ProtoTracer service not found"
            ConnectionObserver.REASON_CANCELLED -> "Connection cancelled"
            FailCallback.REASON_DEVICE_DISCONNECTED -> "Device disconnected during setup"
            else -> {
                if (MAX_CONNECT_ATTEMPTS > 1) {
                    "Connection failed after retries (status $status)"
                } else {
                    "Connection failed (status $status)"
                }
            }
        }
    }

    private fun handleIncoming(bytes: ByteArray) {
        val chunk = bytes.toString(StandardCharsets.UTF_8)
        Log.v(TAG, "[Nordic] handleIncoming: ${bytes.size}b bufferBefore=${txBuffer.length} preview='${chunk.take(64)}'")
        txBuffer.append(chunk)

        while (true) {
            val start = txBuffer.indexOf("{")
            if (start < 0) {
                if (txBuffer.isNotBlank()) txBuffer.clear()
                return
            }

            var depth = 0
            var inString = false
            var escaped = false
            var end = -1

            for (i in start until txBuffer.length) {
                val c = txBuffer[i]
                if (escaped) {
                    escaped = false
                    continue
                }
                if (c == '\\') {
                    escaped = inString
                    continue
                }
                if (c == '"') {
                    inString = !inString
                    continue
                }
                if (inString) continue
                if (c == '{') depth++
                if (c == '}') {
                    depth--
                    if (depth == 0) {
                        end = i + 1
                        break
                    }
                }
            }

            if (end < 0) return
            val json = txBuffer.substring(start, end)
            txBuffer.delete(0, end)

            try {
                val obj = JsonParser.parseString(json).asJsonObject
                Log.d(TAG, "Rx JSON op=${obj.get("op")?.asString ?: "none"}")
                onMessage?.invoke(obj, json)
            } catch (_: Exception) {
                Log.w(TAG, "JSON parse failed: ${json.take(80)}")
            }
        }
    }

    private fun safeDeviceName(device: BluetoothDevice): String? {
        return try {
            device.name
        } catch (_: SecurityException) {
            null
        }
    }

    private fun StringBuilder.isNotBlank(): Boolean = toString().isNotBlank()

    private fun extractOp(json: String): String {
        return try {
            JsonParser.parseString(json).asJsonObject.get("op")?.asString ?: "json"
        } catch (_: Exception) {
            "json"
        }
    }
}