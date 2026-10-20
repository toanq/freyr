package com.synora.smartpot

import android.content.pm.PackageManager
import android.os.Build
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        // Build-time config for Dart (lib/app_config.dart): the API base URL
        // comes from the manifest's synora.api_base_url <meta-data>, filled
        // from the gradle property synoraApiBaseUrl.
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "synora/config").setMethodCallHandler { call, result ->
            when (call.method) {
                "apiBaseUrl" -> result.success(readMetaData("synora.api_base_url"))
                else -> result.notImplemented()
            }
        }
    }

    private fun readMetaData(key: String): String? {
        val info = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            packageManager.getApplicationInfo(
                packageName,
                PackageManager.ApplicationInfoFlags.of(PackageManager.GET_META_DATA.toLong()),
            )
        } else {
            @Suppress("DEPRECATION")
            packageManager.getApplicationInfo(packageName, PackageManager.GET_META_DATA)
        }
        return info.metaData?.getString(key)
    }
}
