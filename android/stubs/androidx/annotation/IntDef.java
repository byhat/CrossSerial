package androidx.annotation;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * Minimal stub of androidx.annotation.IntDef for building the vendored
 * usb-serial-for-android sources with plain javac (no Gradle / AndroidX deps).
 * Mirrors the upstream API surface used by the library.
 */
@Retention(RetentionPolicy.SOURCE)
@Target({ElementType.ANNOTATION_TYPE})
public @interface IntDef {
    int[] value() default {};
    boolean flag() default false;
    boolean open() default false;
}
