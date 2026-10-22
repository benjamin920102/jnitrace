# JNI 攔截擴充版 1.2

## 使用

壓縮檔的 `dist/jnitrace.jar` 同時包含 Windows x64 DLL 和 Linux x64 .so。
Windows x64 的 `dist/jnitrace.dll` 也另外提供。Windows DLL 靜態連結 libffi，無須另裝 libffi DLL。
需要 x64 JDK/JRE 11 或更新版本。Linux 需 libffi.so.8。

```bat
java "-javaagent:C:\tools\jnitrace.jar=module=EveryDay,unknown=1,out=C:\jni-trace,flush=1" -jar target.jar
```

請把 `-javaagent` 加在原本啟動指令的 JVM 參數區。Minecraft 等 launcher 應在 JVM arguments 填入該參數，保留原本 main class、classpath 和遊戲參數。
建議先不設 include，避免漏掉 JNI_OnLoad 或不同 Java 類別的操作。
要縮小範圍可加 `include=your.package.*|other.package.*,depth=8`。
`module=EveryDay` 是 DLL 檔名的不分大小寫子字串；請依實際模組名稱調整。
`unknown=1` 也接受無法解析所屬模組的 caller，可能大幅增加 trace。
輸出：`C:\jni-trace\jni_trace.jsonl`。

啟動時應看到：

```text
[jnitrace] native entry/return: enabled (future binds only)
```

若顯示 DISABLED，代表該 JVM 不提供所需 JVMTI capability；JNI table hooks 仍可用，但該次不會捕捉 Java → native 的入口／返回。

## 攔截範圍

| 類別 | 已安裝 hooks | 記錄重點 |
| --- | --- | --- |
| Java → native | JVMTI NativeMethodBind + libffi wrapper | NativeMethodBind、NativeMethod enter/exit；class、name、sig、static、receiver/class handle、args、回傳值、pending exception、thread、depth、native_parent |
| Native registration | RegisterNatives、UnregisterNatives | class/class handle、count、method name/signature、原始 fn pointer、module、base、RVA、status |
| 類別 | FindClass、GetObjectClass、DefineClass | 名稱、handle；GetObjectClass 的 val 為含 handle 與 class signature 的物件 |
| 方法查詢 | GetMethodID、GetStaticMethodID | class、class handle、name、sig、回傳 methodID |
| 方法呼叫 | Call&lt;Type&gt;Method、CallStatic&lt;Type&gt;Method、CallNonvirtual&lt;Type&gt;Method | Object、Boolean、Byte、Char、Short、Int、Long、Float、Double、Void；一般、V、A 三種；methodID、參數、回傳值；Nonvirtual 另有 dispatch_class |
| Field | GetFieldID、GetStaticFieldID、Get/Set&lt;Type&gt;Field、Get/SetStatic&lt;Type&gt;Field | class、name、sig、fieldID、receiver/class handle、讀取或寫入值；Object 加八種 primitive |
| String | NewString、NewStringUTF、GetStringLength、GetStringUTFLength、GetStringChars、GetStringUTFChars、對應 Release、GetStringRegion、GetStringUTFRegion、Get/ReleaseStringCritical | UTF-16 code units、modified UTF-8、length、region、字串值；critical 只記錄 pointer/handle/isCopy 與 release |
| Array | GetArrayLength、NewObjectArray、Get/SetObjectArrayElement；八種 primitive 的 NewArray、Get/ReleaseArrayElements、Get/SetArrayRegion；Get/ReleasePrimitiveArrayCritical | 長度、index/range、pointer、快照、isCopy、release mode；critical 只記錄 pointer/handle/isCopy 與 release |
| 物件建立 | NewObject、NewObjectV、NewObjectA、AllocObject | class、constructor signature、methodID、args、回傳 handle |
| Exception | Throw、ThrowNew、ExceptionCheck、ExceptionOccurred、ExceptionClear、ExceptionDescribe | Throwable handle、訊息、status、pending_before 與 pending exception |

## Trace 讀法

同一呼叫的 enter/exit 使用相同 `seq`。只有正常返回才會有配對 exit；程序終止或 native crash 可能留下未配對 enter。
`NativeMethod` 的 `va` 是原始實作入口；一般 JNI event 的 `va` 是 native caller 的 return address，並非 call 指令起點。
`base` 是模組載入基址，`rva = va - base`；RegisterNatives 的 `methods[].fn` 是原始 fnPtr，`methods[].rva = fn - base`。
無法解析模組時，base 為 null，rva 不可用。
`depth=0` 的 native event 是追蹤鏈最外層；其內的 JNI event 帶有 `native_parent` 指向該 native enter 的 seq。
Java callback 再進入另一個 native method 時，該 native depth 遞增。這是「已攔截 native」的巢狀深度，不是機器碼 call stack 深度。

預設 compact=1：固定 metadata 放在 context event，enter 使用 ctx 參照。
可展開為一般 JSONL：

```bat
python scripts\expand_trace.py C:\jni-trace\jni_trace.jsonl C:\jni-trace\expanded.jsonl
```

或啟動時設 `compact=0`。`flush=1` 適合 crash 前追蹤，但速度較慢。

## 資料與邊界

- 普通 buffer/array 快照最多 4096 個元素，保留 length/truncated/data，避免無限制輸出；修改 native/jnitrace.cpp 的 SNAP_LIMIT 可調整。它不是完整記憶體 dump。
- GetStringUTFRegion 不假設 NUL 結尾，按 modified UTF-8 的 byte length 記錄 bytes。
- pending exception 時不解讀未定義的正常方法回傳值，exit 標記 exception=true。ExceptionCheck、ExceptionOccurred、Throw/ThrowNew 的有效狀態仍會保留。
- 原始 Critical API 原樣執行。critical region 內只用固定大小 TLS buffer 暫存事件，在最後 release 後輸出，標記 deferred=true；t 是輸出時刻。最多暫存 256 個事件，超量會記 critical_events_dropped。critical buffer 內容不做快照，避免在 pin 期間執行額外 JNI 或阻塞記錄。
- Object handle 是 JNI reference，可能重用或一個物件有多個 handle；不能直接當永久物件 ID。String 參數與回傳可解析為字串，其餘物件不會呼叫 toString。
- NativeMethodBind 攔截初始化之後的新 binding，涵蓋 RegisterNatives 和 Java_* export 的首次解析。agent 啟用前已完成的 binding 不會回補，因此請從啟動時加入 javaagent，不要等載入目標 DLL 後才 attach。
- JNI_OnLoad 本身不屬於 Java 宣告的 native method，沒有 NativeMethod entry；其中通過目前 JNI table 的操作仍可記錄。若目標保留了安裝 hooks 之前的函式指標，該路徑可能漏記。
- 不會解譯 DLL 內部的分支、迴圈、算術、直接記憶體寫入或 Win32/系統 API；只有執行過的路徑才有 trace。本工具提供還原的動態證據，不會自動生成移除 DLL 後的 Java 實作。

## Build 與驗證

Ubuntu：安裝 JDK、g++、make、pkg-config、libffi-dev、g++-mingw-w64-x86-64-posix、Maven，設定 JAVA_HOME 後執行 `bash scripts/build.sh`。
Windows 所需 libffi 3.4.6 原始發行 tarball 與 MIT license 已附於 third_party；build-libffi.sh 檢查 SHA-256 後交叉編譯靜態 libffi。

測試：依 CI workflow 編譯 tests/fixture.cpp 與 tests/probe.cpp 至 target，執行：

```text
python tests/smoke.py
python tests/coverage.py
```

本次驗證：Linux OpenJDK 17.0.20，兩種 JSONL 格式皆通過，且 coverage fixture 在 -Xcheck:jni 下與未啟用 tracer 時輸出相同。
包含雙執行緒、動態與 export native binding、15 個混合型別參數、各種 primitive return、instance native、巢狀 callback/native、arrays、Unicode、pending exception、超界 region、VA/base/RVA 與 seq 配對檢查。
Windows x64 DLL 已交叉編譯，尚未在 Windows JVM 執行驗證。
