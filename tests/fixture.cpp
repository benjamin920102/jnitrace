#include <jni.h>
#include <limits>
extern "C" JNIEXPORT void JNICALL Java_obf_Fixture_run(JNIEnv* e, jclass c) {
    jmethodID echo = e->GetStaticMethodID(c, "echo", "(Ljava/lang/String;)Ljava/lang/String;");
    jchar text[] = {'A',0,0x4e2d,0xd83d,0xde00};
    jstring s = e->NewString(text, 5);
    jobject result = e->CallStaticObjectMethod(c, echo, s);
    e->DeleteLocalRef(result);
    const char* utf = e->GetStringUTFChars(s, nullptr);
    e->ReleaseStringUTFChars(s, utf);
    jfieldID f = e->GetStaticFieldID(c, "value", "I");
    e->SetStaticIntField(c, f, 42);
    (void)e->GetStaticIntField(c, f);
    jmethodID fail = e->GetStaticMethodID(c, "fail", "()V");
    e->CallStaticVoidMethod(c, fail);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(s);
}
