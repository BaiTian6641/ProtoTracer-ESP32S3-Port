package com.prototracer.remote

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.journeyapps.barcodescanner.BarcodeCallback
import com.journeyapps.barcodescanner.BarcodeResult
import com.prototracer.remote.databinding.ActivityQrScannerBinding

/**
 * QR code scanner using ZXing Android Embedded.
 * No Google Play Services dependency.
 * Camera permission is checked in onCreate before initializing the scanner.
 */
class QrScannerActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_QR_RESULT = "qr_result"
    }

    private lateinit var binding: ActivityQrScannerBinding
    private var lastScanned = ""
    private var scanHandled = false
    private var scannerStarted = false

    private val barcodeCallback = BarcodeCallback { result: BarcodeResult ->
        if (scanHandled) return@BarcodeCallback
        val text = result.text
        if (text.isNullOrBlank() || text == lastScanned) return@BarcodeCallback

        scanHandled = true
        lastScanned = text

        // Vibrate to give feedback
        @Suppress("DEPRECATION")
        try {
            val vibrator = getSystemService(VIBRATOR_SERVICE) as? android.os.Vibrator
            vibrator?.vibrate(100)
        } catch (_: Exception) {}

        val intent = android.content.Intent().apply {
            putExtra(EXTRA_QR_RESULT, text)
        }
        setResult(RESULT_OK, intent)
        finish()
    }

    private val cameraPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                startScanner()
            } else {
                Toast.makeText(this, R.string.permission_camera_rationale, Toast.LENGTH_LONG).show()
                setResult(RESULT_CANCELED)
                finish()
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityQrScannerBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.btnCancel.setOnClickListener {
            setResult(RESULT_CANCELED)
            finish()
        }

        // Check camera permission before touching the scanner view
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            == PackageManager.PERMISSION_GRANTED
        ) {
            startScanner()
        } else {
            cameraPermissionLauncher.launch(Manifest.permission.CAMERA)
        }
    }

    private fun startScanner() {
        try {
            binding.barcodeScanner.decodeContinuous(barcodeCallback)
            binding.barcodeScanner.setStatusText(getString(R.string.qr_hint))
            scannerStarted = true
        } catch (e: Exception) {
            Toast.makeText(this, "Camera error: ${e.message}", Toast.LENGTH_LONG).show()
            setResult(RESULT_CANCELED)
            finish()
        }
    }

    override fun onResume() {
        super.onResume()
        if (scannerStarted) {
            try {
                binding.barcodeScanner.resume()
            } catch (_: Exception) {}
        }
    }

    override fun onPause() {
        if (scannerStarted) {
            try {
                binding.barcodeScanner.pause()
            } catch (_: Exception) {}
        }
        super.onPause()
    }

    override fun onDestroy() {
        if (scannerStarted) {
            try {
                binding.barcodeScanner.pause()
            } catch (_: Exception) {}
        }
        super.onDestroy()
    }
}

