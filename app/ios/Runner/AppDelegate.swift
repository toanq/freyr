import Flutter
import UIKit

@main
@objc class AppDelegate: FlutterAppDelegate, FlutterImplicitEngineDelegate {
  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  func didInitializeImplicitFlutterEngine(_ engineBridge: FlutterImplicitEngineBridge) {
    GeneratedPluginRegistrant.register(with: engineBridge.pluginRegistry)

    // Build-time config for Dart (lib/app_config.dart): the API base URL comes
    // from Info.plist's SynoraApiBaseUrl, set via ios/Flutter/Synora.xcconfig.
    if let registrar = engineBridge.pluginRegistry.registrar(forPlugin: "SynoraConfig") {
      let channel = FlutterMethodChannel(name: "synora/config", binaryMessenger: registrar.messenger())
      channel.setMethodCallHandler { call, result in
        if call.method == "apiBaseUrl" {
          result(Bundle.main.object(forInfoDictionaryKey: "SynoraApiBaseUrl") as? String)
        } else {
          result(FlutterMethodNotImplemented)
        }
      }
    }
  }
}
