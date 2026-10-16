// Included by jnitrace.cpp: schema-2 reconstruction evidence, not a bytecode lifter.
static void write_event(const std::string& payload) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_f) { fputs((payload + "\n").c_str(), g_f); if (g_flush_each) fflush(g_f); }
}
static std::mutex g_bindmu;
static std::unordered_map<jmethodID, void*> g_bindings;
static bool native_selected(const MInfo& mi) {
    std::string name = mi.cls;
    if (name.size() > 2 && name[0] == 'L') name = name.substr(1, name.size()-2);
    std::replace(name.begin(), name.end(), '/', '.');
    if (g_classpat.empty()) return true;
    for (const auto& pat : g_classpat) if (glob(pat.c_str(), name.c_str())) return true;
    return false;
}
static std::string method_json(jmethodID m) {
    MInfo mi = minfo(m);
    return "\"method\":" + hexp(m) + ",\"class\":" + jstr(mi.cls.c_str()) +
        ",\"name\":" + jstr(mi.name.c_str()) + ",\"sig\":" + jstr(mi.sig.c_str());
}
struct CallbackScope {
    bool previous;
    CallbackScope() : previous(g_callback) { g_callback = true; }
    ~CallbackScope() { g_callback = previous; }
};
static void JNICALL on_bind(jvmtiEnv*, JNIEnv*, jthread, jmethodID m, void* address, void**) {
    CallbackScope guard;
    { std::lock_guard<std::mutex> lock(g_bindmu); g_bindings[m] = address; }
    if (g_active.load()) write_event("{\"ev\":\"native_bind\"," + method_json(m) +
        ",\"address\":" + hexp(address) + "}");
}
static std::string typed_value(JNIEnv* e, const std::string& t, jvalue v) {
    switch (t.empty() ? 'V' : t[0]) {
        case 'Z': return fv(v.z); case 'B': return fv(v.b); case 'C': return fv(v.c);
        case 'S': return fv(v.s); case 'I': return fv(v.i); case 'J': return fv(v.j);
        case 'F': return fv(v.f); case 'D': return fv(v.d);
        case 'L': case '[': return object_value(e, v.l);
        default: return "null";
    }
}
static bool g_locals = false;
// JVM native frames commonly return OPAQUE_FRAME. Never fabricate argument values.
static std::string entry_args(JNIEnv* e, jthread thread, const MInfo& mi) {
    int index = 0, slot = mi.is_static ? 0 : 1;
    return decode_args(mi.sig, [&](const std::string& t) {
        int current = slot; slot += (t == "J" || t == "D") ? 2 : 1;
        jvalue v{}; jvmtiError error = JVMTI_ERROR_MUST_POSSESS_CAPABILITY;
        if (g_locals) switch (t[0]) {
            case 'L': case '[': error = g_jvmti->GetLocalObject(thread, 0, current, &v.l); break;
            case 'J': error = g_jvmti->GetLocalLong(thread, 0, current, &v.j); break;
            case 'F': error = g_jvmti->GetLocalFloat(thread, 0, current, &v.f); break;
            case 'D': error = g_jvmti->GetLocalDouble(thread, 0, current, &v.d); break;
            default: {
                jint x = 0; error = g_jvmti->GetLocalInt(thread, 0, current, &x);
                switch (t[0]) {
                    case 'Z': v.z = (jboolean)x; break; case 'B': v.b = (jbyte)x; break;
                    case 'C': v.c = (jchar)x; break; case 'S': v.s = (jshort)x; break;
                    default: v.i = x;
                }
            }
        }
        std::string r = "{\"index\":" + std::to_string(index++) + ",\"slot\":" +
            std::to_string(current) + ",\"type\":" + jstr(t.c_str()) + ",\"source\":\"parameter\",\"error\":" +
            std::to_string(error) + ",\"available\":" + (error == JVMTI_ERROR_NONE ? "true" : "false");
        if (error == JVMTI_ERROR_NONE) r += ",\"value\":" + typed_value(e, t, v);
        if (error == JVMTI_ERROR_NONE && (t[0] == 'L' || t[0] == '[') && v.l) g_orig.DeleteLocalRef(e, v.l);
        return r + "}";
    });
}
static void JNICALL on_entry(jvmtiEnv* ti, JNIEnv* e, jthread thread, jmethodID m) {
    if (g_callback || g_thread_ended || !g_active.load()) return;
    jint mods = 0;
    if (ti->GetMethodModifiers(m, &mods) != JVMTI_ERROR_NONE || !(mods & 0x100)) return;
    CallbackScope guard;
    MInfo mi = minfo(m);
    bool selected = native_selected(mi);
    long long id = ++g_frame_seq;
    long long parent = current_native_id();
    g_native_stack.push_back({m, id, selected});
    if (!selected) return;
    std::string receiver = "null";
    jvmtiError receiver_error = JVMTI_ERROR_MUST_POSSESS_CAPABILITY;
    if (!mi.is_static && g_locals) {
        jobject o = nullptr; receiver_error = ti->GetLocalObject(thread, 0, 0, &o);
        if (receiver_error == JVMTI_ERROR_NONE) { receiver = object_value(e, o); if (o) g_orig.DeleteLocalRef(e, o); }
    }
    write_event("{\"ev\":\"native_enter\",\"native_id\":" + std::to_string(id) +
        ",\"parent_native_id\":" + (parent ? std::to_string(parent) : "null") +
        ",\"tid\":" + std::to_string(cur_tid()) + "," + method_json(m) +
        ",\"caller_jni_seq\":" + (g_jni_stack.empty() ? "null" : std::to_string(g_jni_stack.back())) + ",\"static\":" + (mi.is_static ? "true" : "false") + ",\"receiver\":" + receiver +
        ",\"receiver_error\":" + std::to_string(receiver_error) + ",\"args\":" + entry_args(e, thread, mi) + "}");
}
static void JNICALL on_exit(jvmtiEnv*, JNIEnv* e, jthread, jmethodID m, jboolean exception, jvalue v) {
    if (g_callback || g_thread_ended || !g_active.load()) return;
    if (g_native_stack.empty() || g_native_stack.back().method != m) return;
    CallbackScope guard;
    NativeFrame f = g_native_stack.back(); g_native_stack.pop_back();
    if (!f.selected) return;
    MInfo mi = minfo(m);
    std::string out = "{\"ev\":\"native_exit\",\"native_id\":" + std::to_string(f.id) +
        ",\"tid\":" + std::to_string(cur_tid()) + ",\"exception\":" + (exception ? "true" : "false");
    if (!exception && ret_sig(mi.sig) != "V") out += ",\"value\":" + typed_value(e, ret_sig(mi.sig), v);
    write_event(out + "}");
}
static void JNICALL on_thread_end(jvmtiEnv*, JNIEnv*, jthread) {
    // Mark ended before destroying storage so late JNI calls take the bypass.
    g_thread_ended = true;
    InternalScope guard;
    delete g_thread_state;
    g_thread_state = nullptr;
}
static bool g_entry_enabled = false, g_exit_enabled = false, g_bind_enabled = false;
static void configure_live_events() {
    jvmtiCapabilities want{}, have{}, possible{};
    g_jvmti->GetPotentialCapabilities(&possible);
    want.can_tag_objects = possible.can_tag_objects;
    want.can_access_local_variables = possible.can_access_local_variables;
    want.can_generate_native_method_bind_events = possible.can_generate_native_method_bind_events;
    want.can_generate_method_entry_events = possible.can_generate_method_entry_events;
    want.can_generate_method_exit_events = possible.can_generate_method_exit_events;
    g_jvmti->AddCapabilities(&want);
    g_jvmti->GetCapabilities(&have);
    g_tags = have.can_tag_objects;
    g_locals = have.can_access_local_variables;
    g_boundaries = have.can_generate_method_entry_events && have.can_generate_method_exit_events;
    jvmtiEventCallbacks cb{};
    cb.NativeMethodBind = on_bind; cb.MethodEntry = on_entry; cb.MethodExit = on_exit; cb.ThreadEnd = on_thread_end;
    if (g_jvmti->SetEventCallbacks(&cb, sizeof cb) != JVMTI_ERROR_NONE) {
        g_boundaries = false; g_bind_enabled = false; return;
    }
    g_jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_THREAD_END, nullptr);
    if (have.can_generate_native_method_bind_events)
        g_bind_enabled = g_jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_NATIVE_METHOD_BIND, nullptr) == JVMTI_ERROR_NONE;
}
static void enable_boundaries() {
    if (g_boundaries) {
        g_exit_enabled = g_jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_METHOD_EXIT, nullptr) == JVMTI_ERROR_NONE;
        if (g_exit_enabled)
            g_entry_enabled = g_jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_METHOD_ENTRY, nullptr) == JVMTI_ERROR_NONE;
        if (!g_entry_enabled && g_exit_enabled) {
            g_jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_METHOD_EXIT, nullptr); g_exit_enabled = false;
        }
    }
    write_event("{\"ev\":\"capabilities\",\"native_boundaries\":" + std::string(g_entry_enabled && g_exit_enabled ? "true" : "false") +
        ",\"native_bind\":" + (g_bind_enabled ? "true" : "false") + ",\"object_ids\":" + (g_tags ? "true" : "false") +
        ",\"local_access\":" + (g_locals ? "true" : "false") + "}");
}
static void emit_session() {
    write_event("{\"ev\":\"session\",\"schema\":2,\"array_limit\":4096,\"provenance\":\"observed-only\"}");
    std::vector<std::pair<jmethodID, void*>> bindings;
    { std::lock_guard<std::mutex> lock(g_bindmu); for (const auto& b : g_bindings) bindings.push_back(b); }
    for (const auto& b : bindings) write_event("{\"ev\":\"native_bind\"," + method_json(b.first) +
        ",\"address\":" + hexp(b.second) + ",\"buffered\":true}");
}

// Snapshots are bounded. They describe buffer contents, not CPU instructions.
template<class T> static std::string buffer_json(const T* p, jsize count) {
    if (!p || count < 0) return "null";
    std::string out = "{\"length\":" + std::to_string(count) + ",\"truncated\":" +
        (count > 4096 ? "true" : "false") + ",\"values\":[";
    for (jsize i = 0; i < std::min<jsize>(count, 4096); ++i) { if (i) out += ','; out += fv(p[i]); }
    return out + "]}";
}
#define ARRAY_HOOKS(T, R) \
static R##Array JNICALL h_New##T##Array(JNIEnv* e, jsize n) { \
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.New##T##Array(e, n); \
    J j(e, "New" #T "Array", RET_ADDR()); j.n("len", n); j.flush(); \
    auto r = g_orig.New##T##Array(e, n); if (j.on) j.ret_raw(object_value(e, r)); return r; \
} \
static void JNICALL h_Get##T##ArrayRegion(JNIEnv* e, R##Array a, jsize start, jsize n, R* b) { \
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.Get##T##ArrayRegion(e, a, start, n, b); \
    J j(e, "Get" #T "ArrayRegion", RET_ADDR()); j.p("array", (jobject)a); j.n("start", start); j.n("len", n); j.flush(); \
    g_orig.Get##T##ArrayRegion(e, a, start, n, b); \
    if (j.on && !g_orig.ExceptionCheck(e)) j.ret_raw(buffer_json(b, n)); \
} \
static void JNICALL h_Set##T##ArrayRegion(JNIEnv* e, R##Array a, jsize start, jsize n, const R* b) { \
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.Set##T##ArrayRegion(e, a, start, n, b); \
    J j(e, "Set" #T "ArrayRegion", RET_ADDR()); j.p("array", (jobject)a); j.n("start", start); j.n("len", n); \
    if (j.on) j.raw("buffer", buffer_json(b, n)); j.flush(); g_orig.Set##T##ArrayRegion(e, a, start, n, b); \
} \
static R* JNICALL h_Get##T##ArrayElements(JNIEnv* e, R##Array a, jboolean* copy) { \
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.Get##T##ArrayElements(e, a, copy); \
    J j(e, "Get" #T "ArrayElements", RET_ADDR()); j.p("array", (jobject)a); j.flush(); \
    R* r = g_orig.Get##T##ArrayElements(e, a, copy); \
    if (j.on && r && !g_orig.ExceptionCheck(e)) { \
        jsize n = g_orig.GetArrayLength(e, a); \
        j.ret_raw("{\"pointer\":" + hexp(r) + ",\"buffer\":" + buffer_json(r, n) + "}"); \
    } return r; \
} \
static void JNICALL h_Release##T##ArrayElements(JNIEnv* e, R##Array a, R* b, jint mode) { \
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.Release##T##ArrayElements(e, a, b, mode); \
    J j(e, "Release" #T "ArrayElements", RET_ADDR()); j.p("array", (jobject)a); j.p("pointer", (void*)b); j.n("mode", mode); \
    if (j.on && b && !g_orig.ExceptionCheck(e)) j.raw("buffer", buffer_json(b, g_orig.GetArrayLength(e, a))); \
    j.flush(); g_orig.Release##T##ArrayElements(e, a, b, mode); \
}
#define PRIMITIVES(X) X(Boolean, jboolean) X(Byte, jbyte) X(Char, jchar) X(Short, jshort) X(Int, jint) X(Long, jlong) X(Float, jfloat) X(Double, jdouble)
PRIMITIVES(ARRAY_HOOKS)
#define REF_NEW(Name, Type) \
static Type JNICALL h_##Name(JNIEnv* e, jobject o) { \
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.Name(e, o); \
    J j(e, #Name, RET_ADDR()); j.p("obj", o); j.flush(); auto r = g_orig.Name(e, o); \
    if (j.on) j.ret_raw(object_value(e, r)); return r; \
}
REF_NEW(NewLocalRef, jobject)
REF_NEW(NewGlobalRef, jobject)
REF_NEW(NewWeakGlobalRef, jweak)
#define REF_DELETE(Name, Type) \
static void JNICALL h_##Name(JNIEnv* e, Type o) { \
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.Name(e, o); \
    J j(e, #Name, RET_ADDR()); j.p("obj", (jobject)o); j.flush(); g_orig.Name(e, o); \
}
REF_DELETE(DeleteLocalRef, jobject)
REF_DELETE(DeleteGlobalRef, jobject)
REF_DELETE(DeleteWeakGlobalRef, jweak)
static jboolean JNICALL h_IsSameObject(JNIEnv* e, jobject a, jobject b) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.IsSameObject(e, a, b);
    J j(e, "IsSameObject", RET_ADDR()); j.p("left", a); j.p("right", b); j.flush();
    auto r = g_orig.IsSameObject(e, a, b); j.ret_raw(fv(r)); return r;
}
static jint JNICALL h_PushLocalFrame(JNIEnv* e, jint capacity) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.PushLocalFrame(e, capacity);
    J j(e, "PushLocalFrame", RET_ADDR()); j.n("capacity", capacity); j.flush();
    auto r = g_orig.PushLocalFrame(e, capacity); j.ret_raw(fv(r)); return r;
}
static jobject JNICALL h_PopLocalFrame(JNIEnv* e, jobject o) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.PopLocalFrame(e, o);
    J j(e, "PopLocalFrame", RET_ADDR()); j.p("obj", o); j.flush();
    auto r = g_orig.PopLocalFrame(e, o); if (j.on) j.ret_raw(object_value(e, r)); return r;
}
static jsize JNICALL h_GetArrayLength(JNIEnv* e, jarray a) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.GetArrayLength(e, a);
    J j(e, "GetArrayLength", RET_ADDR()); j.p("array", (jobject)a); j.flush();
    auto r = g_orig.GetArrayLength(e, a); j.ret_raw(fv(r)); return r;
}
static jstring JNICALL h_NewString(JNIEnv* e, const jchar* b, jsize n) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.NewString(e, b, n);
    J j(e, "NewString", RET_ADDR()); j.n("len", n); if (j.on) j.raw("utf16", buffer_json(b, n)); j.flush();
    auto r = g_orig.NewString(e, b, n); if (j.on) j.ret_raw(fvo(e, r, "Ljava/lang/String;")); return r;
}
static jsize JNICALL h_GetStringLength(JNIEnv* e, jstring s) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.GetStringLength(e, s);
    J j(e, "GetStringLength", RET_ADDR()); j.p("string", (jobject)s); j.flush();
    auto r = g_orig.GetStringLength(e, s); j.ret_raw(fv(r)); return r;
}
static jsize JNICALL h_GetStringUTFLength(JNIEnv* e, jstring s) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.GetStringUTFLength(e, s);
    J j(e, "GetStringUTFLength", RET_ADDR()); j.p("string", (jobject)s); j.flush();
    auto r = g_orig.GetStringUTFLength(e, s); j.ret_raw(fv(r)); return r;
}
static const jchar* JNICALL h_GetStringChars(JNIEnv* e, jstring s, jboolean* copy) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.GetStringChars(e, s, copy);
    J j(e, "GetStringChars", RET_ADDR()); j.p("string", (jobject)s); j.flush();
    auto r = g_orig.GetStringChars(e, s, copy);
    if (j.on && r && !g_orig.ExceptionCheck(e)) j.ret_raw("{\"pointer\":" + hexp(r) +
        ",\"utf16\":" + buffer_json(r, g_orig.GetStringLength(e, s)) + "}");
    return r;
}
static void JNICALL h_ReleaseStringChars(JNIEnv* e, jstring s, const jchar* b) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.ReleaseStringChars(e, s, b);
    J j(e, "ReleaseStringChars", RET_ADDR()); j.p("string", (jobject)s); j.p("pointer", (void*)b); j.flush();
    g_orig.ReleaseStringChars(e, s, b);
}
static void JNICALL h_ReleaseStringUTFChars(JNIEnv* e, jstring s, const char* b) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.ReleaseStringUTFChars(e, s, b);
    J j(e, "ReleaseStringUTFChars", RET_ADDR()); j.p("string", (jobject)s); j.p("pointer", (void*)b); j.flush();
    g_orig.ReleaseStringUTFChars(e, s, b);
}
static void JNICALL h_GetStringRegion(JNIEnv* e, jstring s, jsize start, jsize n, jchar* b) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.GetStringRegion(e, s, start, n, b);
    J j(e, "GetStringRegion", RET_ADDR()); j.p("string", (jobject)s); j.n("start", start); j.n("len", n); j.flush();
    g_orig.GetStringRegion(e, s, start, n, b);
    if (j.on && !g_orig.ExceptionCheck(e)) j.ret_raw(buffer_json(b, n));
}
static void JNICALL h_GetStringUTFRegion(JNIEnv* e, jstring s, jsize start, jsize n, char* b) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.GetStringUTFRegion(e, s, start, n, b);
    J j(e, "GetStringUTFRegion", RET_ADDR()); j.p("string", (jobject)s); j.n("start", start); j.n("len", n); j.flush();
    g_orig.GetStringUTFRegion(e, s, start, n, b);
    // JNI does not NUL-terminate this buffer. Read exactly the MUTF-8 length derived from original UTF-16 units.
    if (j.on && !g_orig.ExceptionCheck(e) && n >= 0) {
        jsize count = std::min<jsize>(n, 4096);
        std::vector<jchar> chars(count);
        if (count) g_orig.GetStringRegion(e, s, start, count, chars.data());
        size_t bytes = 0; for (jchar c : chars) bytes += (c && c < 0x80) ? 1 : (c < 0x800 ? 2 : 3);
        std::string out = "{\"truncated\":" + std::string(n > count ? "true" : "false") + ",\"bytes\":[";
        for (size_t i=0; i<bytes; ++i) { if (i) out += ','; out += std::to_string((unsigned char)b[i]); }
        j.ret_raw(out + "]}");
    }
}
static jboolean JNICALL h_ExceptionCheck(JNIEnv* e) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.ExceptionCheck(e);
    J j(e, "ExceptionCheck", RET_ADDR()); j.flush(); auto r = g_orig.ExceptionCheck(e);
    j.ret_raw(fv(r), false); return r;
}
static jthrowable JNICALL h_ExceptionOccurred(JNIEnv* e) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.ExceptionOccurred(e);
    J j(e, "ExceptionOccurred", RET_ADDR()); j.flush(); auto r = g_orig.ExceptionOccurred(e);
    if (j.on) j.ret_raw(object_value(e, r, true), false); return r;
}
static void JNICALL h_ExceptionClear(JNIEnv* e) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.ExceptionClear(e);
    J j(e, "ExceptionClear", RET_ADDR()); j.flush(); g_orig.ExceptionClear(e);
}
static jobject JNICALL h_AllocObject(JNIEnv* e, jclass c) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.AllocObject(e, c);
    J j(e, "AllocObject", RET_ADDR()); j.p("class_object", (jobject)c); j.flush();
    auto r = g_orig.AllocObject(e, c); if (j.on) j.ret_raw(object_value(e, r)); return r;
}
static jclass JNICALL h_GetObjectClass(JNIEnv* e, jobject o) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.GetObjectClass(e, o);
    J j(e, "GetObjectClass", RET_ADDR()); j.p("obj", o); j.flush();
    auto r = g_orig.GetObjectClass(e, o); if (j.on) j.ret_raw(object_value(e, r)); return r;
}
static jboolean JNICALL h_IsInstanceOf(JNIEnv* e, jobject o, jclass c) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.IsInstanceOf(e, o, c);
    J j(e, "IsInstanceOf", RET_ADDR()); j.p("obj", o); j.p("class_object", (jobject)c); j.flush();
    auto r = g_orig.IsInstanceOf(e, o, c); j.ret_raw(fv(r)); return r;
}
static jobjectRefType JNICALL h_GetObjectRefType(JNIEnv* e, jobject o) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.GetObjectRefType(e, o);
    J j(e, "GetObjectRefType", RET_ADDR()); j.p("obj", o); j.flush();
    auto r = g_orig.GetObjectRefType(e, o); j.ret_raw(std::to_string((int)r)); return r;
}
static jint JNICALL h_UnregisterNatives(JNIEnv* e, jclass c) {
    if (g_callback || g_thread_ended || !g_active.load(std::memory_order_acquire)) return g_orig.UnregisterNatives(e, c);
    J j(e, "UnregisterNatives", RET_ADDR()); j.p("class_object", (jobject)c); j.flush();
    auto r = g_orig.UnregisterNatives(e, c); j.ret_raw(fv(r)); return r;
}
static void install_extra_hooks() {
#define EXTRA(N) g_new.N = h_##N
#define ARRAY_SET(T, R) EXTRA(New##T##Array); EXTRA(Get##T##ArrayRegion); EXTRA(Set##T##ArrayRegion); EXTRA(Get##T##ArrayElements); EXTRA(Release##T##ArrayElements);
    PRIMITIVES(ARRAY_SET)
    EXTRA(AllocObject); EXTRA(GetObjectClass); EXTRA(IsInstanceOf); EXTRA(GetObjectRefType); EXTRA(UnregisterNatives);
    EXTRA(NewLocalRef); EXTRA(NewGlobalRef); EXTRA(NewWeakGlobalRef);
    EXTRA(DeleteLocalRef); EXTRA(DeleteGlobalRef); EXTRA(DeleteWeakGlobalRef);
    EXTRA(IsSameObject); EXTRA(PushLocalFrame); EXTRA(PopLocalFrame); EXTRA(GetArrayLength);
    EXTRA(NewString); EXTRA(GetStringLength); EXTRA(GetStringUTFLength);
    EXTRA(GetStringChars); EXTRA(ReleaseStringChars); EXTRA(ReleaseStringUTFChars);
    EXTRA(GetStringRegion); EXTRA(GetStringUTFRegion);
    EXTRA(ExceptionCheck); EXTRA(ExceptionOccurred); EXTRA(ExceptionClear);
#undef EXTRA
}
