# JNI entry points are resolved by name, so keep the bridge class and callbacks.
-keep class com.rea.forge.nativebridge.** { *; }
-keepclasseswithmembernames class * { native <methods>; }
