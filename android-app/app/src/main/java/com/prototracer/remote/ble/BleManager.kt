package com.prototracer.remote.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.ParcelUuid
import android.util.Log
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.prototracer.remote.model.BleConstants
import java.nio.charset.StandardCharsets
import java.util.Locale
import java.util.UUID
import kotlin.concurrent.thread

@SuppressLint("MissingPermission")
class BleManager(private val ctx: Context) {

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString(BleConstants.SERVICE_UUID)
        private val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val TAG = "ProtoTracerBle"
        private const val BLE_CHUNK_BYTES = 160
    }

    private val adapter: BluetoothAdapter? =
        (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    private var scanner = adapter?.bluetoothLeScanner
    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var txChar: BluetoothGattCharacteristic? = null
    private var connected = false
    private var intentionalDisconnect = false
    private var currentDeviceName = "ProtoTracer"
    private var currentDeviceAddress = ""
    private var scanNameFilter: String? = null
    private val txBuffer = StringBuilder()
    private val writeLock = Any()

    var onDeviceFound: ((String, String, Int) -> Unit)? = null
    var onConnected: ((String, String) -> Unit)? = null
    var onDisconnected: ((Boolean) -> Unit)? = null
    var onMessage: ((JsonObject, String) -> Unit)? = null
    var onCommandSent: ((String, String) -> Unit)? = null
    var onError: ((String) -> Unit)? = null

    private val scanCb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            try {
                val scanResult = result ?: return
                val device = scanResult.device ?: return
                val address = device.address ?: return
                val scanName = scanResult.scanRecord?.deviceName
                val name = if (!scanName.isNullOrBlank()) scanName else safeDeviceName(device) ?: "ProtoTracer"
                val hasProtoService = scanResult.scanRecord?.serviceUuids?.contains(ParcelUuid(SERVICE_UUID)) == true
                if (!hasProtoService) return

                val filter = scanNameFilter?.lowercase(Locale.US)?.trim().orEmpty()
                if (filter.isNotEmpty() &&
                    !name.lowercase(Locale.US).contains(filter) &&
                    !address.lowercase(Locale.US).contains(filter)) {
                    return
                }

                onDeviceFound?.invoke(name, address, scanResult.rssi)
            } catch (se: SecurityException) {
                onError?.invoke("Bluetooth permission missing while scanning")
            } catch (e: Exception) {
                onError?.invoke("Scan parse error: ${e.message}")
            }
        }

        override fun onScanFailed(errorCode: Int) {
            onError?.invoke("Scan failed: $errorCode")
        }
    }

    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                try {
                    g.requestMtu(247)
                } catch (_: Exception) {
                }
                g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                val wasConnected = connected
                val unexpected = wasConnected && !intentionalDisconnect
                clearGatt()
                intentionalDisconnect = false
                if (wasConnected || status != BluetoothGatt.GATT_SUCCESS) {
                    onDisconnected?.invoke(unexpected)
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                onError?.invoke("Service discovery failed")
                disconnect()
                return
            }

            val service = g.getService(SERVICE_UUID)
            if (service == null) {
                onError?.invoke("ProtoTracer service not found")
                disconnect()
                return
            }

            rxChar = service.characteristics.firstOrNull { ch ->
                (ch.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0
            } ?: service.characteristics.firstOrNull { ch ->
                (ch.properties and BluetoothGattCharacteristic.PROPERTY_WRITE) != 0
            }

            txChar = service.characteristics.firstOrNull { ch ->
                (ch.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY) != 0
            }

            if (rxChar == null || txChar == null) {
                onError?.invoke("Required RX/TX characteristics not found")
                disconnect()
                return
            }

            Log.d(TAG, "RX=${rxChar?.uuid} TX=${txChar?.uuid}")
            enableNotifications(g, txChar!!)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (descriptor.uuid == CCCD_UUID) {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    completeConnection()
                } else {
                    onError?.invoke("Notification setup failed")
                    disconnect()
                }
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            handleIncoming(characteristic.value ?: return)
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            handleIncoming(value)
        }
    }

    fun hasAdapter(): Boolean = adapter != null

    fun isBtEnabled(): Boolean = adapter?.isEnabled == true

    fun isConnected(): Boolean = connected

    fun startScan(nameFilter: String? = null) {
        try {
            scanner = adapter?.bluetoothLeScanner
            scanNameFilter = nameFilter
            stopScan()
            val filters = listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build())
            val settings = ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build()
            scanner?.startScan(filters, settings, scanCb)
        } catch (se: SecurityException) {
            onError?.invoke("Bluetooth scan permission missing")
        } catch (e: Exception) {
            onError?.invoke("Start scan failed: ${e.message}")
        }
    }

    fun stopScan() {
        try {
            scanner?.stopScan(scanCb)
        } catch (_: Exception) {
        }
        scanNameFilter = null
    }

    fun connect(address: String, name: String = "ProtoTracer") {
        try {
            stopScan()
            clearGatt()
            intentionalDisconnect = false
            currentDeviceName = name.ifBlank { "ProtoTracer" }
            currentDeviceAddress = address
            val device = adapter?.getRemoteDevice(address) ?: run {
                onError?.invoke("No Bluetooth adapter")
                return
            }
            gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                device.connectGatt(ctx, false, gattCb, BluetoothDevice.TRANSPORT_LE)
            } else {
                device.connectGatt(ctx, false, gattCb)
            }
        } catch (se: SecurityException) {
            onError?.invoke("Bluetooth connect permission missing")
        } catch (e: Exception) {
            onError?.invoke("Connect failed: ${e.message}")
        }
    }

    fun disconnect() {
        val hadConnection = connected || gatt != null
        intentionalDisconnect = true
        try {
            gatt?.disconnect()
        } catch (_: Exception) {
        }
        clearGatt()
        if (hadConnection) {
            onDisconnected?.invoke(false)
        }
    }

    fun send(json: String) {
        val characteristic = rxChar
        val activeGatt = gatt
        if (!connected || characteristic == null || activeGatt == null) {
            onError?.invoke("Not connected to device")
            return
        }

        thread(name = "ProtoTracerBleWrite") {
            synchronized(writeLock) {
                val data = json.toByteArray(StandardCharsets.UTF_8)
                var offset = 0
                while (offset < data.size) {
                    val end = minOf(offset + BLE_CHUNK_BYTES, data.size)
                    val chunk = data.copyOfRange(offset, end)
                    characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                    val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        activeGatt.writeCharacteristic(
                            characteristic,
                            chunk,
                            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                        ) == BluetoothStatusCodes.SUCCESS
                    } else {
                        @Suppress("DEPRECATION")
                        run {
                            characteristic.value = chunk
                            activeGatt.writeCharacteristic(characteristic)
                        }
                    }
                    if (!ok) {
                        onError?.invoke("BLE write failed")
                        return@thread
                    }
                    offset = end
                    if (offset < data.size) {
                        Thread.sleep(10)
                    }
                }
                onCommandSent?.invoke(extractOp(json), json)
            }
        }
    }

    private fun enableNotifications(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
        val enabled = g.setCharacteristicNotification(characteristic, true)
        if (!enabled) {
            onError?.invoke("Could not enable notifications")
            disconnect()
            return
        }

        val descriptor = characteristic.getDescriptor(CCCD_UUID)
        if (descriptor == null) {
            completeConnection()
            return
        }

        @Suppress("DEPRECATION")
        descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        val writeStarted = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            g.writeDescriptor(descriptor)
        }
        if (!writeStarted) {
            onError?.invoke("Could not write notification descriptor")
            disconnect()
        }
    }

    private fun completeConnection() {
        connected = true
        txBuffer.clear()
        onConnected?.invoke(currentDeviceName, currentDeviceAddress)
    }

    private fun handleIncoming(bytes: ByteArray) {
        val chunk = bytes.toString(StandardCharsets.UTF_8)
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
                onMessage?.invoke(obj, json)
            } catch (e: Exception) {
                Log.w(TAG, "JSON parse failed: ${json.take(80)}")
            }
        }
    }

    private fun clearGatt() {
        try {
            gatt?.close()
        } catch (_: Exception) {
        }
        gatt = null
        rxChar = null
        txChar = null
        connected = false
        txBuffer.clear()
    }

    private fun safeDeviceName(device: BluetoothDevice): String? {
        return try {
            device.name
        } catch (_: SecurityException) {
            null
        }
    }

    private fun StringBuilder.isNotBlank(): Boolean = this.toString().isNotBlank()

    private fun extractOp(json: String): String {
        return try {
            JsonParser.parseString(json).asJsonObject.get("op")?.asString ?: "json"
        } catch (_: Exception) {
            "json"
        }
    }
}