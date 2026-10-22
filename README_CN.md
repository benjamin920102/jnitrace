> 1.2 擴充：新增 Java → native entry/return、primitive arrays、完整傳統 String JNI、exception 查詢、模組 VA/base/RVA。完整用法、限制與測試見 [TRACE_EXTENSIONS_CN.md](TRACE_EXTENSIONS_CN.md)。

# jnitrace：JNI 呼叫追蹤器

原始碼包含 Windows x64 / Linux x64 建置流程，使用 GitHub Actions 的 JDK 21 編譯。
Java agent 的目標版本為 Java 11+。本 ZIP 不附舊版 DLL／SO，請下載 Actions
成功發布的 prerelease 裡的 `jnitrace.jar` 使用，內含新版原生程式庫。

## 使用方式

PowerShell／CMD 請將整段 `-javaagent` 參數加上雙引號：

```powershell
java "-javaagent:jnitrace.jar=include=obf.*;native.*,out=C:\jni-trace" -jar target.jar
```

如果知道 DLL 的實際檔名，可以同時加上模組篩選：

```powershell
java "-javaagent:jnitrace.jar=module=EveryDay,include=obf.*;native.*,depth=4,out=C:\jni-trace" -jar target.jar
```

`module=EveryDay` 比對的是**載入的 DLL 檔名**，不是 JAR 名稱；若 DLL 叫隨機的
`lib123.tmp`，此條件就不會符合。遇到這類情況可只用 `include=`。
同時設定 module 與 include 時，兩者都必須符合。

## 這版改動

- 已攔截的 JNI 呼叫改成 `"ev":"enter"`／`"ev":"exit"`，以相同 `seq` 配對。
- 補上 void 呼叫與其他原本缺少結束事件的 hook；有回傳值的呼叫記錄 `val`。
- JNI 呼叫結束時若存在 Java 例外，記錄 `exception:true`，不將未定義回傳值當成正常結果，也不清除例外。
- 預設 `compact=1`：重複的函式、模組、類別、方法名稱與簽章只記錄一次 context，後續用 `ctx` 參照。
- 只產生 `jni_trace.jsonl`，不再額外產生重複的 JSON 陣列。
- 使用 64 KiB 寫入緩衝，避免每筆 fflush；`flush=1` 可恢復每筆即時落盤。
- Java 字串回傳值不再默默截斷成 400 bytes；支援 NUL、中文與 emoji。
- 保留全部符合篩選的呼叫，不以抽樣、刪除重複事件或截斷值來縮小檔案。

事件例子（`compact=0`）：

```jsonl
{"ev":"enter","seq":1,"tid":123,"fn":"CallStaticIntMethod","class":"Lobf/A;","name":"decode","sig":"(I)I","args":[7]}
{"ev":"exit","seq":1,"val":42}
```

**這裡的 enter／exit 是已攔截的 JNI 函式邊界**，包含 native 透過
`Call*Method` 呼叫 Java；不是每個 Java native 方法本身的 JVMTI 進出事件。
只能追蹤原專案列出的 JNI hook，不能保證還原任意 native 機器碼為完整 Java bytecode。
物件值仍是 JNI handle，不能當成長期穩定的物件 ID。

## 控制輸出大小

| 選項 | 說明 |
| --- | --- |
| `include=obf.*;native.*` | 建議設定。只記錄符合 Java 類別 glob 的呼叫；亦接受 `class=` 與 `\|` 分隔。include 優先。 |
| `module=xxx` | 只記錄來源模組檔名包含此字串的呼叫，不分大小寫。 |
| `depth=1` | 類別比對的頂層 Java stack frame 數量，範圍 1–16；加大可能涵蓋 loader 間接觸發的呼叫。 |
| `compact=1` | 預設使用 context 字典，減少重複中繼資料。 |
| `compact=0` | 每筆完整文字欄位，方便既有解析器讀取，檔案較大。 |
| `flush=1` | 每筆立即 flush，較慢，適合即時觀察或除錯崩潰；不影響檔案大小。 |
| `out=C:\jni-trace` | 指定輸出目錄；預設桌面，沒有桌面則使用家目錄。每次覆寫，請每個程序使用不同目錄。路徑不支援逗號。 |
| `dump=1` | 另外輸出 JNI DefineClass 收到的 class bytes，預設關閉。 |
| `unknown=1` | 使用 module 篩選時，也接收無法辨識模組的記憶體來源；仍須通過類別篩選。 |

不設篩選時，JVM／其他程式庫的 JNI 流量可能非常大。壓縮比例取決於重複中繼資料的數量，
大量不同的實際字串仍會佔空間。字典最多保留 65,536 筆 context，超出後直接輸出完整欄位。
正常關閉會 flush；強制結束或崩潰可能遺失尚未 flush 的尾端，或只剩 enter。

展開 context 為完整文字 JSONL（Python 3）：

```powershell
python scripts/expand_trace.py C:\jni-trace\jni_trace.jsonl C:\jni-trace\expanded.jsonl
python scripts/expand_trace.py C:\jni-trace\jni_trace.jsonl C:\jni-trace\expanded.jsonl.gz
```

工具採串流轉換，可讀取／寫出 `.gz`；請保留原始 context 記錄，否則無法展開。
原本讀取 `ret` 的解析器需改成讀取 `exit`。

## 自動建置／prerelease

將 ZIP 內 `jnitrace/` 的**內容**放到 GitHub repository 根目錄，包含 `.github/`。
工作流：`.github/workflows/prerelease.yml`。

推送到 main／master、推送 v 開頭的 tag，或手動 Run workflow 時：

1. 在 Ubuntu 使用 JDK 21 編譯 Windows x64 DLL、Linux x64 SO 與 JAR。
2. 在 Linux 與 Windows 執行實際 JNI 整合測試。
3. 全部成功後，建立獨立 tag 的 prerelease，附上 `jnitrace.jar` 與 `jnitrace-source.zip`。

PR 只建置測試，不發布。使用內建 GITHUB_TOKEN；release job 已設定 contents:write，
但 repository／組織政策仍需允許該權限。舊 release 不覆寫、不刪除。

完整格式與可選本地建置方式見英文 [README.md](README.md)。
