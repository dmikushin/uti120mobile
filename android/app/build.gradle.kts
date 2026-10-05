import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Release signing key, kept outside the repository:
// ~/.android/uti120-release.properties with storeFile, storePassword, keyAlias, keyPassword.
val releaseKey = Properties().apply {
    val f = File(System.getProperty("user.home"), ".android/uti120-release.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}

android {
    namespace = "io.github.dmikushin.uti120"
    // Compose 1.12 requires compiling against API 37.2.
    compileSdk {
        version = release(37) { minorApiLevel = 2 }
    }
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "io.github.dmikushin.uti120"
        // MediaStore relative paths (Pictures/UTi120, Movies/UTi120) need API 29.
        minSdk = 29
        targetSdk = 37
        versionCode = 6
        versionName = "0.2.0"
        ndk {
            // Phones, and the x86_64 emulator used for testing.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
    }

    signingConfigs {
        if (releaseKey.isNotEmpty()) {
            create("release") {
                storeFile = file(releaseKey.getProperty("storeFile"))
                storePassword = releaseKey.getProperty("storePassword")
                keyAlias = releaseKey.getProperty("keyAlias")
                keyPassword = releaseKey.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        debug {
            // Native code optimised in debug builds too: unoptimised, the
            // backend's per-frame work halves the frame rate (measured in the
            // emulator: 11-12 fps and 10 ms render instead of 21-22 fps and
            // 0.9 ms).  AGP forces CMAKE_BUILD_TYPE=Debug, whose NDK flags set
            // no -O level, so -O2 is added to the flags; -g stays, and Kotlin
            // stays debuggable.
            externalNativeBuild {
                cmake {
                    cFlags += "-O2"
                    cppFlags += "-O2"
                }
            }
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            signingConfig = signingConfigs.findByName("release")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2026.09.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended:1.7.8")
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("androidx.core:core-ktx:1.19.1")
}
