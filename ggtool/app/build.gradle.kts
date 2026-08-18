plugins {
  alias(libs.plugins.android.application)
}

android {
    namespace = "com.ggmcp.tool"
    compileSdk = 36
    defaultConfig {
        applicationId = "com.ggmcp.tool"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
      compose = false
      aidl = false
      buildConfig = false
      shaders = false
    }

    packaging {
      resources {
        excludes += "/META-INF/{AL2.0,LGPL2.1}"
      }
    }
}

dependencies {
  // AndroidX core (minimal; RecyclerView/AppCompat for the Views UI)
  implementation(libs.androidx.core)
  implementation(libs.androidx.appcompat)
  implementation(libs.androidx.recyclerview)
  implementation(libs.androidx.constraintlayout)

  // Local tests: jUnit
  testImplementation(libs.junit)
}
