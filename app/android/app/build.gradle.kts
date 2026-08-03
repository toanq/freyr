plugins {
    id("com.android.application")
    // The Kotlin Gradle Plugin was missing here — without it, .kt files (including
    // MainActivity.kt) are silently never compiled, and the app crashes on launch
    // with ClassNotFoundException since the manifest references a class that was
    // never actually built.
    id("org.jetbrains.kotlin.android")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
    // Reads google-services.json and generates the resources firebase_core needs at build time.
    id("com.google.gms.google-services")
}

android {
    namespace = "com.synora.smartpot"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
        // Required by flutter_local_notifications.
        isCoreLibraryDesugaringEnabled = true
    }

    defaultConfig {
        // TODO: Specify your own unique Application ID (https://developer.android.com/studio/build/application-id.html).
        applicationId = "com.synora.smartpot"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        minSdk = flutter.minSdkVersion
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName
    }

    buildTypes {
        release {
            // TODO: Add your own signing config for the release build.
            // Signing with the debug keys for now, so `flutter run --release` works.
            signingConfig = signingConfigs.getByName("debug")
            // This toolchain's R8 default rules don't keep the manifest-declared
            // MainActivity, which strips it entirely and crashes on launch with
            // ClassNotFoundException. Shrinking isn't needed for this project's
            // testing purposes, so disable it outright rather than hand-author
            // keep rules to work around a toolchain default.
            isMinifyEnabled = false
            isShrinkResources = false
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}

dependencies {
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.1.4")
}
