package com.prototracer.remote.model

/** Represents a scanned BLE device. */
data class BleDevice(
    val name: String,
    val address: String,
    val rssi: Int
)

/** JSON command sent to the firmware RX characteristic. */
data class ControlCommand(
    val op: String = "control.set",
    val expression: Int? = null,
    val brightness: Int? = null,
    val hue_shift: Float? = null,
    val voice_enabled: Boolean? = null,
    val display_mode: Int? = null
)

/** Known BLE service UUID for ProtoTracer firmware. */
object BleConstants {
    const val SERVICE_UUID = "73cf57c7-6797-46e8-8202-dc5e7f956b57"
}
