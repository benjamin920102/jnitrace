// jnitrace.cpp - JNI function-table tracer (loaded from a Java agent via System.load)
// Hooks the JNIEnv function table through JVMTI SetJNIFunctionTable and writes JSONL.
#include <jni.h>
#include <jvmti.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <type_traits>
#include <algorithm>
#include <vector>
#include <cmath>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#define RET_ADDR() __builtin_return_address(0)
#else
#include <dlfcn.h>
#include <pthread.h>
#define RET_ADDR() __builtin_return_address(0)
#endif

// ---------------------------------------------------------------- globals
static jvmtiEnv*          g_jvmti = nullptr;
static JavaVM* g_vm = nullptr;
static JNINativeInterface_ g_orig;
static JNINativeInterface_ g_new;
static FILE*              g_f = nullptr;
static std::atomic<bool> g_active{false};
static bool g_compact = true, g_flush_each = false;
// Protected by g_mu; bounded to avoid unbounded context retention.
static std::unordered_map<std::string, unsigned> g_contexts;
static std::mutex         g_mu;
static std::atomic<long long> g_seq{0};
static std::string        g_filter;      // lower-case substring of module name; empty = all
static std::string        g_dumpdir;     // DefineClass dump dir (optional)
static bool               g_unknown = false;
static std::vector<std::string> g_classpat;  // glob patterns on dotted class name; empty = no class filter
static int                g_depth = 1;       // how many Java frames (from top) to test against g_classpat // log calls whose caller module is unknown

struct ModInfo { bool match; std::string name; void* base = nullptr; };
static std::mutex g_modmu;
static std::unordered_map<void*, ModInfo> g_modcache;

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)tolower(c); });
    return s;
}

static ModInfo mod_of(void* ra) {
    std::string path; void* base = nullptr;
#ifdef _WIN32
    HMODULE h = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ra, &h) && h) {
        char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(h, buf, MAX_PATH);
        path.assign(buf, n); base = (void*)h;
    }
#else
    Dl_info di;
    if (dladdr(ra, &di) && di.dli_fname) { path = di.dli_fname; base = di.dli_fbase; }
#endif
    if (!base) return { g_filter.empty() || g_unknown, "?" };
    {
        std::lock_guard<std::mutex> g(g_modmu);
        auto it = g_modcache.find(base);
        if (it != g_modcache.end()) return it->second;
    }
    size_t p = path.find_last_of("/\\");
    std::string name = p == std::string::npos ? path : path.substr(p + 1);
    bool m = g_filter.empty() || lower(name).find(g_filter) != std::string::npos;
    ModInfo mi{ m, name, base };
    std::lock_guard<std::mutex> g(g_modmu);
    g_modcache[base] = mi;
    return mi;
}

static uint64_t cur_tid() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return (uint64_t)pthread_self();
#endif
}

// ---------------------------------------------------------------- JSON helpers
static void jesc(std::string& o, const char* s) {
    o += '"';
    if (s) for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        unsigned char c = *p;
        if (c >= 0xc0 && c <= 0xdf && p[1] && (p[1] & 0xc0) == 0x80) {
            unsigned cp = ((c & 31) << 6) | (p[1] & 63);
            char b[8]; snprintf(b, sizeof b, "\\u%04x", cp); o += b; ++p; continue;
        }
        if (c >= 0xe0 && c <= 0xef && p[1] && p[2] &&
            (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            unsigned cp = ((c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
            char b[8]; snprintf(b, sizeof b, "\\u%04x", cp); o += b; p += 2; continue;
        }
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    o += '"';
}
static std::string jstr(const char* s) {
    if (!s) return "null";
    std::string o; jesc(o, s); return o;
}
static std::string hexp(const void* p) {
    char b[40]; snprintf(b, sizeof b, "\"0x%llx\"", (unsigned long long)(uintptr_t)p); return b;
}


// ---------------------------------------------------------------- Java-class filter
static bool glob(const char* p, const char* t) {
    if (!*p) return !*t;
    if (*p == '*') { for (;; ++t) { if (glob(p + 1, t)) return true; if (!*t) return false; } }
    return *p == *t && glob(p + 1, t + 1);
}
static std::mutex g_cfmu;
static std::unordered_map<jmethodID, std::string> g_mclass;   // method -> dotted class name
static void release_class(jclass c) {
    JNIEnv* env = nullptr;
    if (c && g_vm && g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK)
        g_orig.DeleteLocalRef(env, c);
}
static std::string dotted_class_of(jmethodID m) {
    { std::lock_guard<std::mutex> g(g_cfmu); auto it = g_mclass.find(m); if (it != g_mclass.end()) return it->second; }
    std::string r; jclass dc = nullptr; char* sg = nullptr;
    if (g_jvmti->GetMethodDeclaringClass(m, &dc) == JVMTI_ERROR_NONE && dc &&
        g_jvmti->GetClassSignature(dc, &sg, nullptr) == JVMTI_ERROR_NONE && sg) {
        r = sg; g_jvmti->Deallocate((unsigned char*)sg);
        if (r.size() > 2 && r[0] == 'L') r = r.substr(1, r.size() - 2);   // Lfoo/Bar; -> foo/Bar
        std::replace(r.begin(), r.end(), '/', '.');
    }
    release_class(dc);
    std::lock_guard<std::mutex> g(g_cfmu); g_mclass[m] = r; return r;
}
// Returns true if one of the top g_depth Java frames of the current thread is in a matching class.
static bool class_match(std::string& who) {
    if (g_classpat.empty()) return true;
    jvmtiFrameInfo fr[16]; jint cnt = 0;
    int d = g_depth < 1 ? 1 : (g_depth > 16 ? 16 : g_depth);
    if (g_jvmti->GetStackTrace(nullptr, 0, d, fr, &cnt) != JVMTI_ERROR_NONE) return false;
    for (jint i = 0; i < cnt; ++i) {
        std::string c = dotted_class_of(fr[i].method);
        for (auto& pat : g_classpat) if (glob(pat.c_str(), c.c_str())) {
            char* mn = nullptr;
            if (g_jvmti->GetMethodName(fr[i].method, &mn, nullptr, nullptr) == JVMTI_ERROR_NONE && mn) {
                who = c + "." + mn; g_jvmti->Deallocate((unsigned char*)mn);
            } else who = c;
            return true;
        }
    }
    return false;
}

// Each intercepted JNI invocation has exactly one enter and one exit on normal return.
// Context definitions are emitted before their first reference, under the writer lock.
struct J {
    bool on = false, done = false, exited = false;
    std::string buf, meta;
    long long seq = 0;
    JNIEnv* env;
    J(JNIEnv* e, const char* fn, void* ra) : env(e) {
        if (!g_active.load(std::memory_order_acquire)) return;
        ModInfo m = mod_of(ra);
        if (!m.match) return;
        std::string who;
        if (!class_match(who)) return;
        on = true; seq = ++g_seq;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
        buf = "{\"ev\":\"enter\",\"seq\":" + std::to_string(seq) +
              ",\"t\":" + std::to_string(ms) + ",\"tid\":" + std::to_string(cur_tid());
        s("fn", fn); s("mod", m.name.c_str());
        // RET_ADDR is the return VA after the intercepted JNI call, not its opcode.
        // Keep addresses inline even in compact mode; hex strings preserve all 64 bits.
        p("va", ra);
        p("dll_base", m.base);
        if (m.base) {
            uintptr_t rva = (uintptr_t)ra - (uintptr_t)m.base;
            // Unlike a null pointer, a zero RVA is a valid offset.
            raw("rva", hexp((const void*)rva));
        } else raw("rva", "null");
        if (!who.empty()) s("in", who.c_str());
    }
    void raw(const char* k, const std::string& v) {
        if (!on) return;
        buf += ",\""; buf += k; buf += "\":"; buf += v;
    }
    void s(const char* k, const char* v) {
        if (!on) return;
        // Only stable metadata is interned. Runtime strings/values remain inline and lossless.
        if (!strcmp(k,"fn") || !strcmp(k,"mod") || !strcmp(k,"in") ||
            !strcmp(k,"class") || !strcmp(k,"name") || !strcmp(k,"sig")) {
            meta += ",\""; meta += k; meta += "\":"; meta += jstr(v);
        } else raw(k, jstr(v));
    }
    void p(const char* k, const void* v) { if (on) raw(k, v ? hexp(v) : "null"); }
    void n(const char* k, long long v) { if (on) raw(k, std::to_string(v)); }
    void flush() {
        if (!on || done) return;
        done = true;
        std::lock_guard<std::mutex> g(g_mu);
        if (!g_f) return;
        if (g_compact) {
            auto it = g_contexts.find(meta);
            if (it == g_contexts.end() && g_contexts.size() < 65536) {
                unsigned id = (unsigned)g_contexts.size() + 1;
                it = g_contexts.emplace(meta, id).first;
                std::string def = "{\"ev\":\"context\",\"id\":" + std::to_string(id) + meta + "}\n";
                fputs(def.c_str(), g_f);
            }
            if (it != g_contexts.end()) buf += ",\"ctx\":" + std::to_string(it->second);
            else buf += meta;
        } else buf += meta;
        buf += "}\n";
        fputs(buf.c_str(), g_f);
        if (g_flush_each) fflush(g_f);
    }
    void ret_raw(const std::string& v) {
        if (!on || exited) return;
        flush(); exited = true;
        std::string l = "{\"ev\":\"exit\",\"seq\":" + std::to_string(seq);
        // Never treat an undefined JNI return value as a real value after an exception.
        if (g_orig.ExceptionCheck(env)) l += ",\"exception\":true";
        else if (!v.empty()) l += ",\"val\":" + v;
        l += "}\n";
        std::lock_guard<std::mutex> g(g_mu);
        if (g_f) { fputs(l.c_str(), g_f); if (g_flush_each) fflush(g_f); }
    }
    ~J() { ret_raw(""); }
};

// ---------------------------------------------------------------- value formatting
static std::string fv(jboolean v) { return v ? "true" : "false"; }
static std::string fv(jbyte v)    { return std::to_string((int)v); }
static std::string fv(jchar v)    { return std::to_string((int)v); }
static std::string fv(jshort v)   { return std::to_string((int)v); }
static std::string fv(jint v)     { return std::to_string((long long)v); }
static std::string fv(jlong v)    { return std::to_string((long long)v); }
static std::string fv(jfloat v)   { char b[48]; snprintf(b, sizeof b, "%.9g", (double)v); return std::isfinite(v) ? std::string(b) : jstr(b); }
static std::string hexp(const void* p);
static std::string fv(jobject v)  { return v ? hexp(v) : "null"; }
static std::string fv(jdouble v)  { char b[48]; snprintf(b, sizeof b, "%.17g", v); return std::isfinite(v) ? std::string(b) : jstr(b); }

// String object -> text (uses ORIGINAL table, never re-enters hooks)
static std::string str_of(JNIEnv* env, jobject o) {
    if (!o) return "null";
    if (g_orig.ExceptionCheck(env)) return hexp(o);
    const jchar* chars = g_orig.GetStringChars(env, (jstring)o, nullptr);
    if (!chars) return hexp(o); // preserve any pending exception
    jsize len = g_orig.GetStringLength(env, (jstring)o);
    std::string text = "\"";
    for (jsize i = 0; i < len; ++i) {
        unsigned c = chars[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') text += (char)c;
        else { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); text += b; }
    }
    text += '"';
    g_orig.ReleaseStringChars(env, (jstring)o, chars);
    return text;
}
static jclass g_strcls = nullptr;   // global ref to java.lang.String
static std::string fvo(JNIEnv* env, jobject o, const char* sig) {
    if (!o) return "null";
    if (g_orig.ExceptionCheck(env)) return hexp(o);
    if (sig && !strcmp(sig, "Ljava/lang/String;")) return str_of(env, o);
    // declared as Object/Comparable/etc: still show the text if it really is a String
    if (g_strcls && g_orig.IsInstanceOf(env, o, g_strcls)) return str_of(env, o);
    return hexp(o);
}

// ---------------------------------------------------------------- JVMTI lookups
struct MInfo { std::string cls, name, sig; bool is_static = false; };
static std::mutex g_cmu;
static std::unordered_map<jmethodID, MInfo> g_mcache;
static std::unordered_map<jfieldID, MInfo> g_fcache;

static std::string sig_of_class(jclass c) {
    char* sg = nullptr; std::string r = "?";
    if (g_jvmti && c && g_jvmti->GetClassSignature(c, &sg, nullptr) == JVMTI_ERROR_NONE && sg) {
        r = sg; g_jvmti->Deallocate((unsigned char*)sg);
    }
    return r;
}
static MInfo minfo(jmethodID m) {
    { std::lock_guard<std::mutex> g(g_cmu); auto it = g_mcache.find(m); if (it != g_mcache.end()) return it->second; }
    MInfo r; r.name = "?"; r.cls = "?";
    char *n = nullptr, *s = nullptr; jclass dc = nullptr;
    if (g_jvmti->GetMethodName(m, &n, &s, nullptr) == JVMTI_ERROR_NONE) {
        r.name = n ? n : "?"; r.sig = s ? s : "";
        if (n) g_jvmti->Deallocate((unsigned char*)n);
        if (s) g_jvmti->Deallocate((unsigned char*)s);
    }
    if (g_jvmti->GetMethodDeclaringClass(m, &dc) == JVMTI_ERROR_NONE && dc) r.cls = sig_of_class(dc);
    release_class(dc);
    jint mod = 0; if (g_jvmti->GetMethodModifiers(m, &mod) == JVMTI_ERROR_NONE) r.is_static = (mod & 0x8) != 0;
    std::lock_guard<std::mutex> g(g_cmu); g_mcache[m] = r; return r;
}
static MInfo finfo(jfieldID f) {
    std::lock_guard<std::mutex> g(g_cmu);
    auto it = g_fcache.find(f); if (it != g_fcache.end()) return it->second;
    MInfo r; r.name = "?"; r.cls = "?"; return r;
}

// Parse "(IJLjava/lang/String;)V" and render args as JSON array.
static std::string ret_sig(const std::string& sig) { size_t p = sig.find(')'); return p == std::string::npos ? "" : sig.substr(p + 1); }
template <class Next>
static std::string decode_args(const std::string& sig, Next next) {
    std::string out = "[";
    size_t i = sig.find('('); if (i == std::string::npos) return "[]"; ++i;
    bool first = true;
    while (i < sig.size() && sig[i] != ')') {
        std::string t; size_t st = i;
        while (sig[i] == '[') ++i;
        if (sig[i] == 'L') i = sig.find(';', i);
        t = sig.substr(st, i - st + 1); ++i;
        if (!first) out += ',';
        first = false;
        out += next(t);
    }
    return out + "]";
}
static std::string args_va(JNIEnv* env, const std::string& sig, va_list ap) {
    va_list c; va_copy(c, ap);
    std::string r = decode_args(sig, [&](const std::string& t) -> std::string {
        switch (t[0]) {
            case 'Z': return fv((jboolean)va_arg(c, int));
            case 'B': return fv((jbyte)va_arg(c, int));
            case 'C': return fv((jchar)va_arg(c, int));
            case 'S': return fv((jshort)va_arg(c, int));
            case 'I': return fv((jint)va_arg(c, int));
            case 'J': return fv((jlong)va_arg(c, long long));
            case 'F': return fv((jfloat)va_arg(c, double));
            case 'D': return fv((jdouble)va_arg(c, double));
            default:  return fvo(env, (jobject)va_arg(c, void*), t.c_str());
        }
    });
    va_end(c); return r;
}
static std::string args_a(JNIEnv* env, const std::string& sig, const jvalue* a) {
    int k = 0;
    return decode_args(sig, [&](const std::string& t) -> std::string {
        const jvalue& v = a[k++];
        switch (t[0]) {
            case 'Z': return fv(v.z); case 'B': return fv(v.b); case 'C': return fv(v.c);
            case 'S': return fv(v.s); case 'I': return fv(v.i); case 'J': return fv(v.j);
            case 'F': return fv(v.f); case 'D': return fv(v.d);
            default:  return fvo(env, v.l, t.c_str());
        }
    });
}

template <class R> static std::string fval(JNIEnv* e, R v, const char* sig) {
    if constexpr (std::is_same_v<R, jobject>) return fvo(e, v, sig); else return fv(v);
}
template <class R> static void log_ret(J& j, JNIEnv* env, const MInfo& mi, R r) {
    if constexpr (std::is_same_v<R, jobject>) j.ret_raw(fvo(env, r, ret_sig(mi.sig).c_str()));
    else j.ret_raw(fv(r));
}

template <class R, class F> static R do_call(J& j, JNIEnv* e, const MInfo& mi, F f) {
    if constexpr (std::is_void_v<R>) { f(); }
    else { R r = f(); if (j.on) log_ret(j, e, mi, r); return r; }
}
struct VaEnd { va_list& a; ~VaEnd() { va_end(a); } };

// ---------------------------------------------------------------- simple hooks
static jclass JNICALL h_FindClass(JNIEnv* e, const char* n) {
    J j(e, "FindClass", RET_ADDR()); j.s("name", n); j.flush();
    jclass r = g_orig.FindClass(e, n); j.ret_raw(hexp(r)); return r;
}
static jclass JNICALL h_DefineClass(JNIEnv* e, const char* n, jobject l, const jbyte* b, jsize len) {
    J j(e, "DefineClass", RET_ADDR()); j.s("name", n); j.n("len", len); j.flush();
    if (j.on && !g_dumpdir.empty() && b && len > 0) {
        std::string nm = n ? n : ("anon_" + std::to_string(j.seq));
        std::replace(nm.begin(), nm.end(), '/', '.');
        std::string path = g_dumpdir + "/" + nm + "_" + std::to_string(j.seq) + ".class";
        if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(b, 1, (size_t)len, f); fclose(f); }
    }
    jclass r = g_orig.DefineClass(e, n, l, b, len); j.ret_raw(fv((jobject)r)); return r;
}
static jmethodID JNICALL h_GetMethodID(JNIEnv* e, jclass c, const char* n, const char* s) {
    J j(e, "GetMethodID", RET_ADDR()); if (j.on) j.s("class", sig_of_class(c).c_str());
    j.s("name", n); j.s("sig", s); j.flush(); jmethodID r = g_orig.GetMethodID(e, c, n, s); j.ret_raw(hexp(r)); return r;
}
static jmethodID JNICALL h_GetStaticMethodID(JNIEnv* e, jclass c, const char* n, const char* s) {
    J j(e, "GetStaticMethodID", RET_ADDR()); if (j.on) j.s("class", sig_of_class(c).c_str());
    j.s("name", n); j.s("sig", s); j.flush(); jmethodID r = g_orig.GetStaticMethodID(e, c, n, s); j.ret_raw(hexp(r)); return r;
}
static jfieldID JNICALL h_GetFieldID(JNIEnv* e, jclass c, const char* n, const char* s) {
    J j(e, "GetFieldID", RET_ADDR()); std::string cs = sig_of_class(c);
    if (j.on) j.s("class", cs.c_str());
    j.s("name", n); j.s("sig", s); j.flush();
    jfieldID r = g_orig.GetFieldID(e, c, n, s);
    if (r) { std::lock_guard<std::mutex> g(g_cmu); g_fcache[r] = { cs, n ? n : "", s ? s : "", false }; }
    j.ret_raw(hexp(r)); return r;
}
static jfieldID JNICALL h_GetStaticFieldID(JNIEnv* e, jclass c, const char* n, const char* s) {
    J j(e, "GetStaticFieldID", RET_ADDR()); std::string cs = sig_of_class(c);
    if (j.on) j.s("class", cs.c_str());
    j.s("name", n); j.s("sig", s); j.flush();
    jfieldID r = g_orig.GetStaticFieldID(e, c, n, s);
    if (r) { std::lock_guard<std::mutex> g(g_cmu); g_fcache[r] = { cs, n ? n : "", s ? s : "", true }; }
    j.ret_raw(hexp(r)); return r;
}
static jstring JNICALL h_NewStringUTF(JNIEnv* e, const char* s) {
    J j(e, "NewStringUTF", RET_ADDR()); j.s("str", s); j.flush(); jstring r = g_orig.NewStringUTF(e, s); j.ret_raw(fv((jobject)r)); return r;
}
static const char* JNICALL h_GetStringUTFChars(JNIEnv* e, jstring s, jboolean* c) {
    J j(e, "GetStringUTFChars", RET_ADDR()); j.flush();
    const char* r = g_orig.GetStringUTFChars(e, s, c);
    if (j.on) j.ret_raw(g_orig.ExceptionCheck(e) ? "" : jstr(r));
    return r;
}
static jint JNICALL h_RegisterNatives(JNIEnv* e, jclass c, const JNINativeMethod* m, jint n) {
    J j(e, "RegisterNatives", RET_ADDR());
    if (j.on) {
        j.s("class", sig_of_class(c).c_str());
        std::string a = "[";
        for (jint i = 0; i < n; ++i) {
            if (i) a += ',';
            a += "{\"name\":" + jstr(m[i].name) + ",\"sig\":" + jstr(m[i].signature) + ",\"fn\":" + hexp(m[i].fnPtr) + "}";
        }
        j.raw("methods", a + "]");
    }
    j.flush(); jint r = g_orig.RegisterNatives(e, c, m, n); j.ret_raw(fv(r)); return r;
}
static jint JNICALL h_ThrowNew(JNIEnv* e, jclass c, const char* msg) {
    J j(e, "ThrowNew", RET_ADDR()); j.s("class", sig_of_class(c).c_str()); j.s("msg", msg); j.flush();
    jint r = g_orig.ThrowNew(e, c, msg); j.ret_raw(fv(r)); return r;
}
static jint JNICALL h_Throw(JNIEnv* e, jthrowable t) {
    J j(e, "Throw", RET_ADDR()); j.p("obj", t); j.flush(); jint r = g_orig.Throw(e, t); j.ret_raw(fv(r)); return r;
}

// ---------------------------------------------------------------- Call*Method families
// instance: (env,obj,mid,...)  static: (env,cls,mid,...)  nonvirtual: (env,obj,cls,mid,...)
#define CALL_FAMILY(T, R, KIND, KINDNAME, CT)                                                       \
static R JNICALL h_Call##KIND##T##Method(JNIEnv* e, CT o, jmethodID m, ...) {                    \
    J j(e, "Call" KINDNAME #T "Method", RET_ADDR()); MInfo mi;                                           \
    va_list ap; va_start(ap, m);                                                                      \
    if (j.on) { mi = minfo(m); j.s("class", mi.cls.c_str()); j.s("name", mi.name.c_str());            \
        j.s("sig", mi.sig.c_str()); j.raw("args", args_va(e, mi.sig, ap)); j.p("obj", o); }           \
    j.flush();                                                                                        \
    VaEnd ve{ap}; return do_call<R>(j, e, mi, [&]{ return g_orig.Call##KIND##T##MethodV(e, o, m, ap); });  \
}                                                                                                     \
static R JNICALL h_Call##KIND##T##MethodV(JNIEnv* e, CT o, jmethodID m, va_list ap) {            \
    J j(e, "Call" KINDNAME #T "MethodV", RET_ADDR()); MInfo mi;                                          \
    if (j.on) { mi = minfo(m); j.s("class", mi.cls.c_str()); j.s("name", mi.name.c_str());            \
        j.s("sig", mi.sig.c_str()); j.raw("args", args_va(e, mi.sig, ap)); j.p("obj", o); }           \
    j.flush();                                                                                        \
    return do_call<R>(j, e, mi, [&]{ return g_orig.Call##KIND##T##MethodV(e, o, m, ap); });  \
}                                                                                                     \
static R JNICALL h_Call##KIND##T##MethodA(JNIEnv* e, CT o, jmethodID m, const jvalue* a) {       \
    J j(e, "Call" KINDNAME #T "MethodA", RET_ADDR()); MInfo mi;                                          \
    if (j.on) { mi = minfo(m); j.s("class", mi.cls.c_str()); j.s("name", mi.name.c_str());            \
        j.s("sig", mi.sig.c_str()); j.raw("args", args_a(e, mi.sig, a)); j.p("obj", o); }             \
    j.flush();                                                                                        \
    return do_call<R>(j, e, mi, [&]{ return g_orig.Call##KIND##T##MethodA(e, o, m, a); });  \
}

#define CALL_FAMILY_NV(T, R)                                                                          \
static R JNICALL h_CallNonvirtual##T##Method(JNIEnv* e, jobject o, jclass c, jmethodID m, ...) {      \
    J j(e, "CallNonvirtual" #T "Method", RET_ADDR()); MInfo mi;                                          \
    va_list ap; va_start(ap, m);                                                                      \
    if (j.on) { mi = minfo(m); j.s("class", mi.cls.c_str()); j.s("name", mi.name.c_str());            \
        j.s("sig", mi.sig.c_str()); j.raw("args", args_va(e, mi.sig, ap)); j.p("obj", o); }           \
    j.flush();                                                                                        \
    VaEnd ve{ap}; return do_call<R>(j, e, mi, [&]{ return g_orig.CallNonvirtual##T##MethodV(e, o, c, m, ap); });  \
}                                                                                                     \
static R JNICALL h_CallNonvirtual##T##MethodV(JNIEnv* e, jobject o, jclass c, jmethodID m, va_list ap) { \
    J j(e, "CallNonvirtual" #T "MethodV", RET_ADDR()); MInfo mi;                                         \
    if (j.on) { mi = minfo(m); j.s("class", mi.cls.c_str()); j.s("name", mi.name.c_str());            \
        j.s("sig", mi.sig.c_str()); j.raw("args", args_va(e, mi.sig, ap)); j.p("obj", o); }           \
    j.flush();                                                                                        \
    return do_call<R>(j, e, mi, [&]{ return g_orig.CallNonvirtual##T##MethodV(e, o, c, m, ap); });  \
}                                                                                                     \
static R JNICALL h_CallNonvirtual##T##MethodA(JNIEnv* e, jobject o, jclass c, jmethodID m, const jvalue* a) { \
    J j(e, "CallNonvirtual" #T "MethodA", RET_ADDR()); MInfo mi;                                         \
    if (j.on) { mi = minfo(m); j.s("class", mi.cls.c_str()); j.s("name", mi.name.c_str());            \
        j.s("sig", mi.sig.c_str()); j.raw("args", args_a(e, mi.sig, a)); j.p("obj", o); }             \
    j.flush();                                                                                        \
    return do_call<R>(j, e, mi, [&]{ return g_orig.CallNonvirtual##T##MethodA(e, o, c, m, a); });  \
}

#define ALL_TYPES(X) X(Object, jobject) X(Boolean, jboolean) X(Byte, jbyte) X(Char, jchar) \
    X(Short, jshort) X(Int, jint) X(Long, jlong) X(Float, jfloat) X(Double, jdouble) X(Void, void)
#define INST(T, R)  CALL_FAMILY(T, R, , "", jobject)
#define STAT(T, R)  CALL_FAMILY(T, R, Static, "Static", jclass)
ALL_TYPES(INST)
ALL_TYPES(STAT)
ALL_TYPES(CALL_FAMILY_NV)

// NewObject family: (env, cls, mid, ...)
static jobject JNICALL h_NewObject(JNIEnv* e, jclass c, jmethodID m, ...) {
    J j(e, "NewObject", RET_ADDR()); MInfo mi; va_list ap; va_start(ap, m);
    if (j.on) { mi = minfo(m); j.s("class", mi.cls.c_str()); j.s("sig", mi.sig.c_str()); j.raw("args", args_va(e, mi.sig, ap)); }
    j.flush(); jobject r = g_orig.NewObjectV(e, c, m, ap); va_end(ap); j.ret_raw(fv(r)); return r;
}
static jobject JNICALL h_NewObjectV(JNIEnv* e, jclass c, jmethodID m, va_list ap) {
    J j(e, "NewObjectV", RET_ADDR()); MInfo mi;
    if (j.on) { mi = minfo(m); j.s("class", mi.cls.c_str()); j.s("sig", mi.sig.c_str()); j.raw("args", args_va(e, mi.sig, ap)); }
    j.flush(); jobject r = g_orig.NewObjectV(e, c, m, ap); j.ret_raw(fv(r)); return r;
}
static jobject JNICALL h_NewObjectA(JNIEnv* e, jclass c, jmethodID m, const jvalue* a) {
    J j(e, "NewObjectA", RET_ADDR()); MInfo mi;
    if (j.on) { mi = minfo(m); j.s("class", mi.cls.c_str()); j.s("sig", mi.sig.c_str()); j.raw("args", args_a(e, mi.sig, a)); }
    j.flush(); jobject r = g_orig.NewObjectA(e, c, m, a); j.ret_raw(fv(r)); return r;
}

// ---------------------------------------------------------------- field accessors
#define FIELD_HOOKS(T, R)                                                                             \
static R JNICALL h_Get##T##Field(JNIEnv* e, jobject o, jfieldID f) {                                  \
    J j(e, "Get" #T "Field", RET_ADDR()); MInfo fi;                                                      \
    if (j.on) { fi = finfo(f); j.s("class", fi.cls.c_str()); j.s("name", fi.name.c_str()); j.p("obj", o); } \
    j.flush(); R r = g_orig.Get##T##Field(e, o, f); if (j.on) log_ret(j, e, fi, r); return r;         \
}                                                                                                     \
static void JNICALL h_Set##T##Field(JNIEnv* e, jobject o, jfieldID f, R v) {                          \
    J j(e, "Set" #T "Field", RET_ADDR());                                                                \
    if (j.on) { MInfo fi = finfo(f); j.s("class", fi.cls.c_str()); j.s("name", fi.name.c_str());      \
        j.s("sig", fi.sig.c_str()); j.p("obj", o);                                                    \
        j.raw("val", fval<R>(e, v, fi.sig.c_str())); } \
    j.flush(); g_orig.Set##T##Field(e, o, f, v);                                                      \
}                                                                                                     \
static R JNICALL h_GetStatic##T##Field(JNIEnv* e, jclass c, jfieldID f) {                             \
    J j(e, "GetStatic" #T "Field", RET_ADDR()); MInfo fi;                                                \
    if (j.on) { fi = finfo(f); j.s("class", fi.cls.c_str()); j.s("name", fi.name.c_str()); }          \
    j.flush(); R r = g_orig.GetStatic##T##Field(e, c, f); if (j.on) log_ret(j, e, fi, r); return r;   \
}                                                                                                     \
static void JNICALL h_SetStatic##T##Field(JNIEnv* e, jclass c, jfieldID f, R v) {                     \
    J j(e, "SetStatic" #T "Field", RET_ADDR());                                                          \
    if (j.on) { MInfo fi = finfo(f); j.s("class", fi.cls.c_str()); j.s("name", fi.name.c_str());      \
        j.s("sig", fi.sig.c_str());                                                                   \
        j.raw("val", fval<R>(e, v, fi.sig.c_str())); } \
    j.flush(); g_orig.SetStatic##T##Field(e, c, f, v);                                                \
}
#define FIELD_TYPES(X) X(Object, jobject) X(Boolean, jboolean) X(Byte, jbyte) X(Char, jchar) \
    X(Short, jshort) X(Int, jint) X(Long, jlong) X(Float, jfloat) X(Double, jdouble)
FIELD_TYPES(FIELD_HOOKS)


// ---------------------------------------------------------------- object arrays (string tables!)
static jobject JNICALL h_GetObjectArrayElement(JNIEnv* e, jobjectArray a, jsize i) {
    J j(e, "GetObjectArrayElement", RET_ADDR()); j.p("array", a); j.n("index", i); j.flush();
    jobject r = g_orig.GetObjectArrayElement(e, a, i);
    if (j.on) j.ret_raw(fvo(e, r, "Ljava/lang/Object;"));
    return r;
}
static void JNICALL h_SetObjectArrayElement(JNIEnv* e, jobjectArray a, jsize i, jobject v) {
    J j(e, "SetObjectArrayElement", RET_ADDR()); j.p("array", a); j.n("index", i);
    if (j.on) j.raw("val", fvo(e, v, "Ljava/lang/Object;"));
    j.flush(); g_orig.SetObjectArrayElement(e, a, i, v);
}
static jobjectArray JNICALL h_NewObjectArray(JNIEnv* e, jsize n, jclass c, jobject init) {
    J j(e, "NewObjectArray", RET_ADDR()); j.n("len", n); j.s("elem", sig_of_class(c).c_str()); j.flush();
    jobjectArray r = g_orig.NewObjectArray(e, n, c, init); if (j.on) j.ret_raw(hexp(r)); return r;
}

// ---------------------------------------------------------------- install
#define SET(name) g_new.name = h_##name
#define SET_CALLS(T, R) \
    g_new.Call##T##Method = h_Call##T##Method; g_new.Call##T##MethodV = h_Call##T##MethodV; g_new.Call##T##MethodA = h_Call##T##MethodA; \
    g_new.CallStatic##T##Method = h_CallStatic##T##Method; g_new.CallStatic##T##MethodV = h_CallStatic##T##MethodV; g_new.CallStatic##T##MethodA = h_CallStatic##T##MethodA; \
    g_new.CallNonvirtual##T##Method = h_CallNonvirtual##T##Method; g_new.CallNonvirtual##T##MethodV = h_CallNonvirtual##T##MethodV; g_new.CallNonvirtual##T##MethodA = h_CallNonvirtual##T##MethodA;
#define SET_FIELDS(T, R) \
    g_new.Get##T##Field = h_Get##T##Field; g_new.Set##T##Field = h_Set##T##Field; \
    g_new.GetStatic##T##Field = h_GetStatic##T##Field; g_new.SetStatic##T##Field = h_SetStatic##T##Field;

extern "C" JNIEXPORT jboolean JNICALL
Java_jnitrace_Natives_init(JNIEnv* env, jclass, jstring jpath, jstring jfilter, jstring jdump, jboolean unknown, jstring jclasses, jint depth, jboolean compact, jboolean flushEach) {
    if (g_f) return g_active.load() ? JNI_TRUE : JNI_FALSE;
    JavaVM* vm = nullptr; env->GetJavaVM(&vm); g_vm = vm;
    if (vm->GetEnv((void**)&g_jvmti, JVMTI_VERSION_1_2) != JNI_OK || !g_jvmti) return JNI_FALSE;

    const char* p = env->GetStringUTFChars(jpath, nullptr);
    g_f = fopen(p, "wb"); env->ReleaseStringUTFChars(jpath, p);
    if (!g_f) return JNI_FALSE;
    if (jfilter) { const char* f = env->GetStringUTFChars(jfilter, nullptr); g_filter = lower(f); env->ReleaseStringUTFChars(jfilter, f); }
    if (jdump)   { const char* d = env->GetStringUTFChars(jdump, nullptr);   g_dumpdir = d;        env->ReleaseStringUTFChars(jdump, d); }
    g_unknown = unknown; g_depth = depth; g_compact = compact; g_flush_each = flushEach;
    setvbuf(g_f, nullptr, _IOFBF, 64 * 1024);
    if (jclasses) {
        const char* c = env->GetStringUTFChars(jclasses, nullptr);
        std::string all = c; env->ReleaseStringUTFChars(jclasses, c);
        size_t i = 0;
        while (i <= all.size()) {
            size_t j2 = all.find_first_of("|;", i); if (j2 == std::string::npos) j2 = all.size();
            std::string t = all.substr(i, j2 - i);
            if (!t.empty()) g_classpat.push_back(t);
            i = j2 + 1;
        }
    }

    { jclass sc = env->FindClass("java/lang/String"); if (sc) { g_strcls = (jclass)env->NewGlobalRef(sc); env->DeleteLocalRef(sc); } }
    JNINativeInterface_* cur = nullptr;
    if (g_jvmti->GetJNIFunctionTable(&cur) != JVMTI_ERROR_NONE || !cur) return JNI_FALSE;
    g_orig = *cur; g_jvmti->Deallocate((unsigned char*)cur);
    g_new = g_orig;

    SET(FindClass); SET(DefineClass); SET(GetMethodID); SET(GetStaticMethodID);
    SET(GetFieldID); SET(GetStaticFieldID); SET(NewStringUTF); SET(GetStringUTFChars);
    SET(RegisterNatives); SET(ThrowNew); SET(Throw);
    SET(NewObject); SET(NewObjectV); SET(NewObjectA);
    SET(GetObjectArrayElement); SET(SetObjectArrayElement); SET(NewObjectArray);
    ALL_TYPES(SET_CALLS)
    FIELD_TYPES(SET_FIELDS)

    g_active.store(true, std::memory_order_release);
    if (g_jvmti->SetJNIFunctionTable(&g_new) == JVMTI_ERROR_NONE) return JNI_TRUE;
    g_active.store(false, std::memory_order_release);
    fclose(g_f); g_f = nullptr;
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_jnitrace_Natives_shutdown(JNIEnv*, jclass) {
    g_active.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> g(g_mu);
    if (g_f) { fflush(g_f); }   // keep hooks alive; file closed by process exit
}
