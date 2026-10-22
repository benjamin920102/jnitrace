
#include <jni.h>
#include <cstdlib>
static void check(bool b){if(!b)std::abort();}
static double JNICALL mixed(JNIEnv*,jclass,jboolean z,jbyte b,jchar c,jshort s,jint i,jlong j,jfloat f,jdouble d,jstring text,jint k,jdouble x,jlong y,jfloat v,jint w,jdouble q){
 check(z && b==-7 && c==0x4e2d && s==-300 && i==1234 && j==9000000000LL && f==1.25f && d==2.5 && text && k==9 && x==4.5 && y==8000000000LL && v==2.25f && w==-17 && q==8.5);return 101.25;
}
static jlong JNICALL child(JNIEnv*,jclass,jlong x){return x+1;}
static void JNICALL thrower(JNIEnv* e,jclass){e->ThrowNew(e->FindClass("java/lang/IllegalStateException"),"expected");}
static jboolean JNICALL retBoolean(JNIEnv*,jclass,jboolean x){return x;}
static jbyte JNICALL retByte(JNIEnv*,jclass,jbyte x){return x;}
static jchar JNICALL retChar(JNIEnv*,jclass,jchar x){return x;}
static jshort JNICALL retShort(JNIEnv*,jclass,jshort x){return x;}
static jint JNICALL retInt(JNIEnv*,jclass,jint x){return x;}
static jfloat JNICALL retFloat(JNIEnv*,jclass,jfloat x){return x;}
static jdouble JNICALL retDouble(JNIEnv*,jclass,jdouble x){return x;}
static jlong JNICALL instance(JNIEnv*,jobject,jlong x){return x-1;}
extern "C" JNIEXPORT void JNICALL Java_obf_Probe_install(JNIEnv* e,jclass c){
 JNINativeMethod m[]={{(char*)"mixed",(char*)"(ZBCSIJFDLjava/lang/String;IDJFID)D",(void*)mixed},{(char*)"child",(char*)"(J)J",(void*)child},{(char*)"thrower",(char*)"()V",(void*)thrower}};
 check(e->RegisterNatives(c,m,3)==0);
 JNINativeMethod extra[]={{(char*)"retBoolean",(char*)"(Z)Z",(void*)retBoolean},{(char*)"retByte",(char*)"(B)B",(void*)retByte},{(char*)"retChar",(char*)"(C)C",(void*)retChar},{(char*)"retShort",(char*)"(S)S",(void*)retShort},{(char*)"retInt",(char*)"(I)I",(void*)retInt},{(char*)"retFloat",(char*)"(F)F",(void*)retFloat},{(char*)"retDouble",(char*)"(D)D",(void*)retDouble},{(char*)"instance",(char*)"(J)J",(void*)instance}};
 check(e->RegisterNatives(c,extra,8)==0);
}
extern "C" JNIEXPORT void JNICALL Java_obf_Probe_exercise(JNIEnv* e,jclass c){
 jclass pc=e->FindClass("obf/Probe");jmethodID ctor=e->GetMethodID(pc,"<init>","()V");
 jobject o=e->NewObject(pc,ctor);check(o);jclass oc=e->GetObjectClass(o);check(oc);
 jobject blank=e->AllocObject(pc);check(blank);
 jfieldID field=e->GetFieldID(pc,"field","I");check(e->GetIntField(o,field)==7);e->SetIntField(o,field,99);check(e->GetIntField(o,field)==99);
 jfieldID sf=e->GetStaticFieldID(pc,"text","Ljava/lang/String;");jobject text=e->GetStaticObjectField(pc,sf);e->SetStaticObjectField(pc,sf,text);
 jmethodID cb=e->GetStaticMethodID(c,"callback","()J");check(e->CallStaticLongMethod(c,cb)==1234567890124LL);check(!e->ExceptionCheck());
 jchar units[]={'A',0,0x4e2d,0xd83d,0xde00};jstring str=e->NewString(units,5);check(e->GetStringLength(str)==5);check(e->GetStringUTFLength(str)==12);
 const jchar* chars=e->GetStringChars(str,nullptr);check(chars[2]==0x4e2d);e->ReleaseStringChars(str,chars);
 jchar region[3];e->GetStringRegion(str,1,3,region);check(!e->ExceptionCheck());check(region[0]==0);
 char bytes[20];e->GetStringUTFRegion(str,0,5,bytes);check(!e->ExceptionCheck());
 const char* utf=e->GetStringUTFChars(str,nullptr);e->ReleaseStringUTFChars(str,utf);
 const jchar* pin=e->GetStringCritical(str,nullptr);check(pin[0]=='A');e->ReleaseStringCritical(str,pin);
 jobjectArray oa=e->NewObjectArray(2,pc,o);check(e->GetArrayLength(oa)==2);check(e->GetObjectArrayElement(oa,0));e->SetObjectArrayElement(oa,1,blank);
#define ARRAY(T,R,V) {R vals[]={V,(R)2,(R)3};R##Array a=e->New##T##Array(3);e->Set##T##ArrayRegion(a,0,3,vals);R out[3];e->Get##T##ArrayRegion(a,0,3,out);check(!e->ExceptionCheck());check(out[0]==vals[0]);R* p=e->Get##T##ArrayElements(a,nullptr);p[1]=(R)1;e->Release##T##ArrayElements(a,p,JNI_COMMIT);e->Release##T##ArrayElements(a,p,0);e->DeleteLocalRef(a);}
 ARRAY(Boolean,jboolean,1) ARRAY(Byte,jbyte,-12) ARRAY(Char,jchar,0x4e2d) ARRAY(Short,jshort,-1234)
 ARRAY(Int,jint,-123456) ARRAY(Long,jlong,9000000000LL) ARRAY(Float,jfloat,1.25f) ARRAY(Double,jdouble,2.5)
 jintArray a=e->NewIntArray(2);jint* p=(jint*)e->GetPrimitiveArrayCritical(a,nullptr);p[0]=9;e->ReleasePrimitiveArrayCritical(a,p,0);
 jint val;e->GetIntArrayRegion(a,0,1,&val);check(!e->ExceptionCheck());check(val==9);
 e->SetIntArrayRegion(a,500,100000,nullptr);check(e->ExceptionCheck());e->ExceptionClear();
 jclass exc=e->FindClass("java/lang/IllegalArgumentException");e->ThrowNew(exc,"cleared");check(e->ExceptionCheck());jthrowable ex=e->ExceptionOccurred();check(ex);e->ExceptionClear();check(!e->ExceptionCheck());e->Throw(ex);check(e->ExceptionCheck());e->ExceptionClear();
 e->DeleteLocalRef(o);e->DeleteLocalRef(blank);e->DeleteLocalRef(pc);e->DeleteLocalRef(oc);e->DeleteLocalRef(str);e->DeleteLocalRef(oa);e->DeleteLocalRef(a);e->DeleteLocalRef(exc);e->DeleteLocalRef(ex);e->DeleteLocalRef(text);
}
