# JNI 還原追蹤增強版（原始碼）

本次依要求沒有編譯、沒有執行測試；ZIP 不包含預先編譯的 DLL、SO 或 JAR。

## 改了什麼

- 保留 JNI enter/exit 與 compact context；增加 schema=2、native_id、parent_seq。
- 加入 JVMTI MethodEntry/MethodExit：只對 native 方法建立每執行緒巢狀堆疊，記錄正常回傳與異常退出。Java callback 再進入 native 時另有 caller_jni_seq。
- 加入 NativeMethodBind：包含靜態 JNI 符號解析綁定，保留 RegisterNatives 原本的記錄。啟動早期的綁定會暫存到正式輸出開始；不是掃描已經綁定的方法。
- 參數記錄 index、JVM slot、descriptor、parameter 來源標籤、available 與 JVMTI error。long/double 佔兩個 slot；非 static receiver 另外記錄。無法讀取時不產生虛構值。
- 使用這個 JVMTI environment 的 object tag 提供 object_id。同一物件的 local/global/weak reference 可有相同 ID；不持有額外強引用阻止 GC。不同程序／不同追蹤 session 的 ID 不可比較。
- 補齊 8 種 primitive array 的建立、region 讀寫、elements 取得與釋放。snapshot 上限 4096 個元素，有 truncated 標記；保留 JNI_ABORT/JNI_COMMIT 的 mode，snapshot 不等於已寫回 Java array。
- 補 local/global/weak reference 建立與刪除、local frame、IsSameObject、GetObjectRefType、AllocObject、GetObjectClass、IsInstanceOf、GetArrayLength、UnregisterNatives。
- 補 UTF-16 字串建立、長度、chars／release、region／UTF region；UTF region 不假設 NUL 結尾。物件值使用 {ref, object_id, identity_error}，字串額外有 text。
- 補 ExceptionCheck／Occurred／Clear；ExceptionOccurred 的 throwable 使用物件 ID。一般 JNI 呼叫發生 pending exception 時，不把未定義的回傳值當成結果。
- scripts/trace_to_operations.py 將 JNI 配對轉為 operation，保留輸入、輸出、例外、巢狀關係及起迄順序。未返回的操作標示 incomplete。

## 建議啟動方式

先自行依原專案的 scripts/build.sh 建置。完整 native boundary capability 應於 JVM 啟動時申請：

Linux（替換實際絕對路徑）：

```sh
java -agentpath:/absolute/path/libjnitrace.so=log=/absolute/path/jni_trace.jsonl,module=target,include=your.package.*,flush=1 -jar target.jar
```

Windows x64（整個帶空白的 agent 參數須加雙引號）：

```bat
java "-agentpath:C:\tools\jnitrace.dll=log=C:\trace\jni_trace.jsonl,module=target,include=your.package.*,flush=1" -jar target.jar
```

- 此模式不用再加 -javaagent；native library 直接提供 Agent_OnLoad。
- log 是完整輸出檔名，預設為工作目錄下 jni_trace.jsonl。父目錄請事先建立。
- 支援 module、include（多個 glob 用 | 或 ;）、unknown、compact、flush、dumpdir。dumpdir 要先建立，僅用於既有 DefineClass dump。
- native_id 指向最近一層符合 include 的 native 邊界，沒有可用邊界時為 null；被過濾的 frame 仍在內部堆疊維護。
- module 過濾 JNI caller；native boundary 只按 include 過濾，綁定事件不套 module 過濾，確保保留方法對應資料。
- 原本 -javaagent:jnitrace.jar=out=... 的使用方式仍保留，但 live phase 可能拿不到 method entry/exit 或 locals capability，且無法補錄初始化前已經發生的綁定。
- capabilities 事件的 native_boundaries、native_bind、object_ids、local_access 才是此次實際啟用狀態。啟動時寫入此資訊不代表測試已通過。

轉為操作事件：

```sh
python scripts/trace_to_operations.py jni_trace.jsonl operations.jsonl
```

操作於 exit 到來時輸出；需要原始先後順序時請用 start_order/end_order，不能只依輸出行號。原始 trace 請保留。兩個 Python 轉換器均支援 .gz。

## 還原能力的界線

這是「還原器的觀測輸入」增強，不是 c2j 等級的 bytecode lifter，也沒有直接輸出還原 JAR 的功能。

- 多數 JVM 不允許 GetLocal* 讀 native frame，回傳 OPAQUE_FRAME。此版本會記錄 unavailable/error；若要每次都取得完整入口參數，仍需 ABI 正確的 native trampoline 或其他 instrumentation。
- parameter 來源、物件 ID 與 JNI 操作串接只是已觀測的關聯；沒有追蹤 CPU 算術、native 分支、資料相依或 taint，也無法得知未執行路徑。
- 尚未攔截 Get/ReleasePrimitiveArrayCritical、Get/ReleaseStringCritical、DirectByteBuffer、Monitor 或所有 JNI API。Critical 區域有禁止 JNI 呼叫的限制，不能直接套一般 logger。
- 不攔截純 native 記憶體逐次讀寫；elements release 只提供當下 buffer snapshot，且超過上限會截斷。
- DefineClass dump 只能收集傳入 JVM 的 class bytes，不能恢復 class 裡 native 方法的實作。
- MethodEntry/Exit 是全域 JVM 事件，繁忙程式可能有明顯額外負擔。執行緒堆疊使用 trivial TLS pointer 並在 JVMTI ThreadEnd 清理；VM 結束、強制中止或追蹤開始前已在執行的方法可能沒有完整配對。
- 方法／field metadata cache 沿用原專案模式，沒有新增 class unload/redefinition 的失效機制；重載類別的長時間追蹤需要進一步處理。
- schema=2 的物件值改為結構化資料，舊版只接受 handle／純字串的消費端須更新。保留的 smoke.py 已按新格式調整，但未執行，也不等於完整新功能驗證。

修改檔案：native/jnitrace.cpp、native/reconstruction_hooks.inc、native/startup_agent.inc、scripts/trace_to_operations.py、tests/smoke.py、README.md、README_CN.md 與本說明。

## Windows heap corruption 修訂

使用者提供的退出碼 3221226356（0xC0000374）表示 Windows heap corruption。
只有 subprocess 的 traceback 無法確定哪一行 native 程式先破壞 heap；以下是針對已發現風險的修訂，沒有聲稱在 Windows 重現或驗證成功。

- 所有 JNI hook 加入原始函式直接轉送路徑，涵蓋 logger 內部、追蹤停止與 ThreadEnd 後的呼叫。variadic JNI 呼叫使用 V 版本正確轉送。
- class filter、JVMTI metadata、字串／object_id 格式化及返回事件檢查加入 InternalScope；logger 內部造成的 JNI／callback 不再重入記錄流程。實際目標 JNI 呼叫仍允許合法 callback 與 native 巢狀呼叫。
- 過濾掉的呼叫不再無條件做物件結果格式化／tag 查詢。
- 移除含 destructor 的 thread_local vector；改用 trivial TLS pointer，JVMTI ThreadEnd 先停用該執行緒追蹤，再釋放堆疊。這是 teardown 防護，不是已證實的此次 root cause。若 JVM 拒絕 ThreadEnd 通知，該執行緒 storage 會保留到程序結束。
- smoke 仍檢查原有呼叫數、配對、字串、例外及過濾行為；不跳過 Windows job。
- smoke 不再用 TemporaryDirectory 刪除故障現場，改保留 target/smoke-traces，啟用 flush=1，記錄 Java stdout/stderr、十六進位退出碼，以及 JVM 能產生時的 hs_err 報告。CI 失敗時上傳診斷 artifacts。

依先前要求，此修訂也沒有編譯或執行測試。如仍發生 0xC0000374，下一步需要 Windows diagnostics artifact／crash dump 才能繼續定位原生 heap 損壞來源。

## CI 更新：依要求停用 Python 測試

GitHub Actions 只保留編譯與 prerelease；移除 setup-python、Linux smoke、windows-test job、fixture 建置與 smoke diagnostics 上傳。prerelease 只依賴 build。
測試原始碼仍保留於 tests/，workflow 不再執行。Python 事件轉換工具仍可手動使用。
這個設定取代前一節的 CI smoke／診斷上傳安排；停止測試不代表 Windows runtime 崩潰已驗證解決。
