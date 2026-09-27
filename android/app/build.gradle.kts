plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "io.github.zengarv.deuxdisplay"
    compileSdk = 35

    defaultConfig {
        applicationId = "io.github.zengarv.deuxdisplay"
        minSdk = 26
        targetSdk = 35
        // The release workflow passes these from the git tag (-PversionName=0.2.0 -PversionCode=200).
        versionCode = providers.gradleProperty("versionCode").orNull?.toInt() ?: 1
        versionName = providers.gradleProperty("versionName").orNull ?: "0.1.0"
    }

    // Release signing comes from the environment (CI secrets); without it, release APKs are unsigned.
    val keystore = System.getenv("DEUXDISPLAY_KEYSTORE")
    signingConfigs {
        if (keystore != null) {
            create("release") {
                storeFile = file(keystore)
                storePassword = System.getenv("DEUXDISPLAY_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("DEUXDISPLAY_KEY_ALIAS")
                keyPassword = System.getenv("DEUXDISPLAY_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            if (keystore != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
        allWarningsAsErrors = true
    }

    lint {
        abortOnError = true
        checkReleaseBuilds = true
    }
}

dependencies {
    testImplementation(libs.junit)
}
