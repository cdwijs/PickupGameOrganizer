plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "org.pgo.artip2p"
    compileSdk = 34

    defaultConfig {
        applicationId = "org.pgo.artip2p"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
        // Only ship ABIs we build the Rust .so for (see README: cargo-ndk step).
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    // The native library is placed in src/main/jniLibs/<abi>/libarti_p2p_android.so
    // by the cargo-ndk build step and packaged automatically.
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.4")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
