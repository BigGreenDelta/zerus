# Optional Firebase entry point is called reflectively only in private Firebase builds.
-keep class app.zerus.mobile.OptionalFirebase { public static *; }

# The task replica's UniFFI bindings call the Rust library through JNA, which finds
# its classes and the bindings' structures by reflection.
-keep class com.sun.jna.** { *; }
-keep class * implements com.sun.jna.** { *; }
-keep class app.zerus.mobile.replica.** { *; }
-dontwarn java.awt.**
