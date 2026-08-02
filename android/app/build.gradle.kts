plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "dev.rikkahub.ce"
    compileSdk = 34

    signingConfigs {
        create("release") {
            // CI 注入正式 keystore（GitHub Secrets）；本地无 env 时退回 debug 签名
            val ksB64 = System.getenv("RIKKA_CE_KEYSTORE_B64")
            if (ksB64 != null) {
                val ksFile = File(
                    System.getenv("RUNNER_TEMP") ?: "/tmp",
                    "rikkahub-ce.keystore",
                )
                ksFile.writeBytes(java.util.Base64.getDecoder().decode(ksB64))
                storeFile = ksFile
                storePassword = System.getenv("RIKKA_CE_KEYSTORE_PASS")
                keyAlias = System.getenv("RIKKA_CE_KEY_ALIAS") ?: "rikkahub-ce"
                keyPassword = System.getenv("RIKKA_CE_KEY_PASS")
            }
        }
    }

    defaultConfig {
        applicationId = "dev.rikkahub.ce"
        minSdk = 26
        targetSdk = 34
        versionCode = 2
        versionName = "0.2.0"
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                cppFlags += ""
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // 统一签名：CI 有正式 keystore 用正式签名；本地退回 debug
            signingConfig = if (signingConfigs.getByName("release").storeFile != null) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2024.06.00"))
    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.3")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.3")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("androidx.documentfile:documentfile:1.0.1")
}
