package io.github.dmikushin.uti120

import android.content.Intent
import android.hardware.usb.UsbManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge

/**
 * The live screen.  Portrait-locked like the vendor's MainActivity; it is also
 * started by the system when the camera is plugged in (USB_DEVICE_ATTACHED).
 */
class MainActivity : ComponentActivity() {
    private lateinit var controller: CameraController

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        controller = CameraController(applicationContext)
        // Debug builds can run on recorded frames:
        //   am start -n io.github.dmikushin.uti120/.MainActivity -e replay /sdcard/Android/data/io.github.dmikushin.uti120/files/replay.raw
        if (BuildConfig.DEBUG) controller.replay = intent.getStringExtra("replay")
        controller.register()
        setContent { CameraScreen(controller) }
    }

    override fun onStart() {
        super.onStart()
        controller.connect()
    }

    override fun onStop() {
        super.onStop()
        controller.disconnect()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) controller.connect()
    }

    override fun onDestroy() {
        controller.unregister()
        controller.shutdown()
        super.onDestroy()
    }
}
