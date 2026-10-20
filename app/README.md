# synora_app

A new Flutter project.

## Getting Started

This project is a starting point for a Flutter application.

A few resources to get you started if this is your first Flutter project:

- [Learn Flutter](https://docs.flutter.dev/get-started/learn-flutter)
- [Write your first Flutter app](https://docs.flutter.dev/get-started/codelab)
- [Flutter learning resources](https://docs.flutter.dev/reference/learning-resources)

For help getting started with Flutter development, view the
[online documentation](https://docs.flutter.dev/), which offers tutorials,
samples, guidance on mobile development, and a full API reference.

## Docker dev environment

`docker-compose.dev.yml` runs a long-lived Flutter/Android container so the
Gradle daemon and Android SDK components stay warm across builds instead of
being re-downloaded/re-JIT'd by every one-shot `docker run --rm` (a cold
build takes ~70s just for Gradle; a warm one is ~4s). It uses host
networking, so `adb`/`flutter run` inside the container see the same devices
`adb` sees on the host (USB or Wi-Fi).

Bring it up once:

```bash
cd app
docker compose -f docker-compose.dev.yml up -d
```

Then run Flutter commands against it as needed:

```bash
# Debug build
docker compose -f docker-compose.dev.yml exec flutter-dev flutter build apk --debug

# Release build
docker compose -f docker-compose.dev.yml exec flutter-dev flutter build apk --release

# Hot reload against a connected/paired device (see `adb devices` for the id)
docker compose -f docker-compose.dev.yml exec flutter-dev flutter run -d <device-id>
```

Install the result the normal way: `adb install -r build/app/outputs/flutter-apk/app-debug.apk`.

Caches persisted on the host so they survive container recreation:

- `~/.pub-cache` → pub package cache
- `~/.gradle-cache-host` → Gradle dependency + daemon cache
- `~/.android-debug-keystore-host` → the debug signing keystore — without
  this, every fresh container mints a new debug key and the resulting APK
  can't update whatever's currently installed on the phone
  (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`)

Tear down with `docker compose -f docker-compose.dev.yml down` (the host
cache directories are untouched by this).

## Server URL

The app reads its API base URL (default `https://synora.id.vn/api`) from
platform build metadata at startup (`lib/app_config.dart`), not from Dart
code. Everything that needs the server -- API calls and the `server_url` sent
to a device during BLE pairing -- uses that one value. Invalid or missing
values (must be an absolute `https://` URL) fall back to the default.

- **Android**: `synoraApiBaseUrl` in `android/gradle.properties`, baked into
  the manifest as `<meta-data android:name="synora.api_base_url">`. Override
  for one build without editing files:

  ```bash
  flutter build apk --release --android-project-arg synoraApiBaseUrl=https://example.com/api
  # or: ORG_GRADLE_PROJECT_synoraApiBaseUrl=https://example.com/api flutter build apk --release
  ```

  Check an APK with
  `aapt2 dump xmltree app-release.apk --file AndroidManifest.xml | grep -A2 synora.api_base_url`.
- **iOS**: `SYNORA_API_BASE_URL` in `ios/Flutter/Synora.xcconfig`, which feeds
  the `SynoraApiBaseUrl` key in `Info.plist`. Write `https:/$()/host/api` --
  `//` starts a comment in xcconfig files.

The value is fixed at build time. Changing it in an already-built APK means
decoding, editing and repackaging it (e.g. with apktool) and re-signing it --
not a code change, but still a new APK.

Release APKs currently sign with the **debug** keystore too (see
`android/app/build.gradle.kts`'s `release` buildType) — a deliberate stopgap,
not yet a proper release-signing setup.
