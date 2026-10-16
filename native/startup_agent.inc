// Startup mode owns one JVMTI environment; the Java-agent path reuses it if present.
static std::unordered_map<std::string, std::string> g_start_options;
static void JNICALL startup_vm_init(jvmtiEnv*, JNIEnv* e, jthread) {
    CallbackScope guard;
    auto get = [](const char* key, const char* fallback) {
        auto it = g_start_options.find(key); return it == g_start_options.end() ? std::string(fallback) : it->second;
    };
    std::string log = get("log", "jni_trace.jsonl"), module = get("module", ""), classes = get("include", ""), dump = get("dumpdir", "");
    jstring p = e->NewStringUTF(log.c_str()), m = e->NewStringUTF(module.c_str());
    jstring c = e->NewStringUTF(classes.c_str()), d = dump.empty() ? nullptr : e->NewStringUTF(dump.c_str());
    if (e->ExceptionCheck() || !p || !m || !c) {
        fprintf(stderr, "[jnitrace] startup option conversion failed\n"); return;
    }
    bool ok = Java_jnitrace_Natives_init(e, nullptr, p, m, d, get("unknown", "0") == "1", c,
        1, get("compact", "1") != "0", get("flush", "0") == "1");
    e->DeleteLocalRef(p); e->DeleteLocalRef(m); e->DeleteLocalRef(c); if (d) e->DeleteLocalRef(d);
    if (!ok) fprintf(stderr, "[jnitrace] startup failed; check output directory and JVM capabilities\n");
}
extern "C" JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void*) {
    g_vm = vm;
    if (vm->GetEnv((void**)&g_jvmti, JVMTI_VERSION_1_2) != JNI_OK) return JNI_ERR;
    std::string all = options ? options : "";
    size_t begin = 0;
    while (begin < all.size()) {
        size_t end = all.find(',', begin); if (end == std::string::npos) end = all.size();
        std::string item = all.substr(begin, end-begin); size_t eq = item.find('=');
        if (eq != std::string::npos) g_start_options[item.substr(0, eq)] = item.substr(eq+1);
        begin = end+1;
    }
    jvmtiCapabilities possible{}, want{};
    if (g_jvmti->GetPotentialCapabilities(&possible) != JVMTI_ERROR_NONE) return JNI_ERR;
    want.can_tag_objects = possible.can_tag_objects;
    want.can_access_local_variables = possible.can_access_local_variables;
    want.can_generate_native_method_bind_events = possible.can_generate_native_method_bind_events;
    want.can_generate_method_entry_events = possible.can_generate_method_entry_events;
    want.can_generate_method_exit_events = possible.can_generate_method_exit_events;
    if (g_jvmti->AddCapabilities(&want) != JVMTI_ERROR_NONE) return JNI_ERR;
    jvmtiEventCallbacks cb{}; cb.VMInit = startup_vm_init; cb.NativeMethodBind = on_bind;
    if (g_jvmti->SetEventCallbacks(&cb, sizeof cb) != JVMTI_ERROR_NONE) return JNI_ERR;
    if (possible.can_generate_native_method_bind_events &&
        g_jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_NATIVE_METHOD_BIND, nullptr) != JVMTI_ERROR_NONE) return JNI_ERR;
    return g_jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, nullptr) == JVMTI_ERROR_NONE ? JNI_OK : JNI_ERR;
}
extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM*) {
    g_active.store(false);
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_f) fflush(g_f);
}
