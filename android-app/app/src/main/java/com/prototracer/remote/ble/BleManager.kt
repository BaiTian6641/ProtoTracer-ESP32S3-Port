package com.prototracer.remote.ble

import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.os.ParcelUuid
import android.util.Log
import com.prototracer.remote.model.BleConstants
import java.util.UUID

/**
 * Minimal BLE manager: scan, connect, find RX char by UUID, send JSON commands.
 * No TX, no notifications, no manifest — write-only.
 */
class BleManager(private val ctx: Context) {

    companion object {
        val SERVICE_UUID = UUID.fromString(BleConstants.SERVICE_UUID)
        private const val TAG = "BleMan"
    }

    private val adapter: BluetoothAdapter? =
        (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    private var scanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null

    var onDeviceFound: ((String, String) -> Unit)? = null      // name, address
    var onConnected: (() -> Unit)? = null
    var onDisconnected: (() -> Unit)? = null
    var onError: ((String) -> Unit)? = null

    private val scanCb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, r: ScanResult?) {
            try {
                val d = r?.device ?: return
                val addr = d.address ?: return
                val localName = r.scanRecord?.deviceName
                val name = if (!localName.isNullOrBlank()) localName else "ProtoTracer"

                // Prefer strict matching by service UUID to avoid unrelated devices with similar names.
                val hasProtoService = r.scanRecord?.serviceUuids?.contains(ParcelUuid(SERVICE_UUID)) == true
                if (hasProtoService) {
                    onDeviceFound?.invoke(name, addr)
                }
            } catch (se: SecurityException) {
                onError?.invoke("Bluetooth permission missing while scanning")
            } catch (e: Exception) {
                onError?.invoke("Scan parse error: ${e.message}")
            }
        }
        override fun onScanFailed(err: Int) { onError?.invoke("Scan failed: $err") }
    }

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                g.discoverServices()
            } else {
                gatt = null; rxChar = null; onDisconnected?.invoke()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) { onError?.invoke("Service discovery failed"); return }
            val svc = g.getService(SERVICE_UUID)
            if (svc == null) { onError?.invoke("ProtoTracer service not found"); return }
            rxChar = null
            for (ch in svc.characteristics) {
                if ((ch.properties and BluetoothGattCharacteristic.PROPERTY_WRITE) != 0 ||
                    (ch.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0) {
                    rxChar = ch; break
                }
            }
            if (rxChar == null) { onError?.invoke("RX characteristic not found"); return }
            Log.d(TAG, "RX char: ${rxChar!!.uuid}")
            onConnected?.invoke()
        }
    }

    fun isBtEnabled() = adapter?.isEnabled == true

    fun startScan() {
        try {
            scanner = adapter?.bluetoothLeScanner ?: return
            stopScan()
            scanner?.startScan(null, ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scanCb)
        } catch (se: SecurityException) {
            onError?.invoke("Bluetooth scan permission missing")
        } catch (e: Exception) {
            onError?.invoke("Start scan failed: ${e.message}")
        }
    }

    fun stopScan() {
        try {
            scanner?.stopScan(scanCb)
        } catch (_: Exception) {}
    }

    fun connect(address: String) {
        try {
            val dev = adapter?.getRemoteDevice(address) ?: return
            gatt = dev.connectGatt(ctx, false, gattCb)
        } catch (se: SecurityException) {
            onError?.invoke("Bluetooth connect permission missing")
        } catch (e: Exception) {
            onError?.invoke("Connect failed: ${e.message}")
        }
    }

    fun disconnect() { gatt?.disconnect(); gatt?.close(); gatt = null; rxChar = null }

    /** Send JSON to the firmware RX characteristic. 160-byte chunks, WRITE_TYPE_NO_RESPONSE. */
    fun send(json: String) {
        val ch = rxChar ?: return
        val g = gatt ?: return
        val data = json.toByteArray()
        ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        var off = 0
        while (off < data.size) {
            val len = minOf(160, data.size - off)
            ch.value = data.copyOfRange(off, off + len)
            if (!g.writeCharacteristic(ch)) { Thread.sleep(15); continue }
            off += len
            if (off < data.size) Thread.sleep(2)
        }
        Log.d(TAG, "sent: ${json.take(80)}")
    }
}
