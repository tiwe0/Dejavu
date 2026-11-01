# Keep stack traces readable in internal crash reports.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile

# AndroidX/Compose are consumed as AARs with their own rules. Keep annotations
# used by Kotlin metadata and the platform JSON bridge.
-keepattributes *Annotation*
-dontwarn org.jetbrains.annotations.**
