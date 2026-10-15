# jnitrace

JNI function-table tracer bundled as a Java agent. The build includes Windows x64
and Linux x64 libraries. Build with JDK 21; the Java agent targets Java 11+.
No precompiled libraries are included in this source package.

## Use

Download `jnitrace.jar` from a successful GitHub prerelease. It contains both native
libraries; there is no need to configure `java.library.path`.

PowerShell / CMD (quote the complete agent argument):

```powershell
java "-javaagent:jnitrace.jar=include=obf.*;native.*,out=C:\jni-trace" -jar target.jar
```

To require both a module and a Java class match:

```powershell
java "-javaagent:jnitrace.jar=module=EveryDay,include=obf.*;native.*,depth=4,out=C:\jni-trace" -jar target.jar
```

Use `module=` only when you know the loaded native module's filename. A random
`lib*.tmp` filename will not match `module=EveryDay`. Class filtering is usually
more useful in that case. Patterns match dotted, case-sensitive class names.
Both filters must pass when both are set. Without either filter, JVM/library JNI
traffic can produce very large files.

| Option | Default | Meaning |
| --- | --- | --- |
| `include=obf.*;native.*` | all | Class globs, separated by `;` or `\|`. `class=` is an alias. `include=` takes precedence. |
| `depth=1` | 1 | Match the top 1–16 Java stack frames. Larger depths can include loader-triggered JNI traffic. |
| `module=xxx` | all | Case-insensitive substring of the native caller's module filename. |
| `unknown=1` | 0 | Include anonymous caller memory when filtering by module. Class filtering still applies. |
| `out=directory` | desktop, else home | Directory containing `jni_trace.jsonl`; each run overwrites it. Use a separate directory per process/run. Commas in paths are unsupported. |
| `compact=1` | 1 | Store repeated function/module/class/method metadata in context records. No event sampling or deduplication. |
| `compact=0` | — | Inline full metadata in every enter record; easier for existing readers. |
| `flush=1` | 0 | Flush each record for live tailing/crash investigations. Default: 64 KiB stdio buffer, flushed on normal shutdown. |
| `dump=1` | 0 | Dump bytes passed to intercepted JNI `DefineClass` calls into `defined_classes/`. |

## Events and size reduction

Only `jni_trace.jsonl` is generated. The previous redundant `jni_trace.json` array
and shutdown conversion have been removed. The 64 KiB buffer reduces write/flush
overhead; buffering alone does not reduce file size. Compact contexts remove repeated
metadata, but the savings depend on workload. Filtering reduces the captured scope.
Actual arguments, return values and invocation counts are not sampled or dropped.
Context retention is capped at 65,536 entries; further new contexts are inlined.
Strings returned as Java objects are no longer silently truncated to 400 bytes.
Consequently, workloads with huge unique strings can still generate large files.

Plain mode example:

```jsonl
{"ev":"enter","seq":1,"t":1234567890,"tid":123,"fn":"CallStaticIntMethod","mod":"target.dll","class":"Lobf/A;","name":"decode","sig":"(I)I","args":[7]}
{"ev":"exit","seq":1,"val":42}
```

Compact mode example:

```jsonl
{"ev":"context","id":1,"fn":"CallStaticIntMethod","mod":"target.dll","class":"Lobf/A;","name":"decode","sig":"(I)I"}
{"ev":"enter","seq":1,"t":1234567890,"tid":123,"ctx":1,"args":[7]}
{"ev":"exit","seq":1,"val":42}
```

`seq` uniquely pairs enter/exit across threads. Context definitions precede their
references in the same file. Exit inherits the function/thread information from its
enter record. Void calls have no `val`. A pending Java exception produces
`"exception":true` and omits the undefined return value; tracing does not clear it.
Non-finite floating values are JSON strings, such as `"nan"` or `"inf"`.
Object references are JNI handles, not persistent object identities.

These are boundaries of **intercepted JNI functions**, including native-to-Java
`Call*Method` calls. They are not JVMTI MethodEntry/MethodExit events around every
Java-to-native method invocation. A process crash, forced termination, non-returning
call or concurrent JVM shutdown can leave an unmatched enter or buffered tail.

Covered hooks: FindClass, DefineClass, method/field ID lookup, Call*Method (instance,
static, nonvirtual; variadic/V/A), NewObject, field access, NewStringUTF,
GetStringUTFChars, RegisterNatives, Throw/ThrowNew and object-array access/allocation.
Other JNI functions are not intercepted. JNI tracing is not native machine-code
lifting and does not reconstruct arbitrary native code into complete Java bytecode.

Expand compact traces using Python 3 (streaming, bounded by the context table):

```powershell
python scripts/expand_trace.py C:\jni-trace\jni_trace.jsonl C:\jni-trace\expanded.jsonl
# Optional gzip output for smaller archived/expanded traces:
python scripts/expand_trace.py C:\jni-trace\jni_trace.jsonl C:\jni-trace\expanded.jsonl.gz
```

The expander removes `context` records and replaces `ctx` with its full metadata.
Do not remove the dictionary records before expanding. It also accepts `.gz` input.
For parsers expecting string metadata, expand first or capture with `compact=0`.
Event consumers must use `exit` instead of the old `ret` event name.

## GitHub Actions: automatic prerelease

The workflow is `.github/workflows/prerelease.yml`. Place this project's contents
(including `.github/`) at your repository root.

- Push to `main` / `master`, push a `v*` tag, or select **Run workflow**.
- Ubuntu builds both native libraries from source and packages the agent using JDK 21.
- Linux and Windows integration tests check pairing, exceptions, Unicode, threads,
  class filters, compact expansion and output size.
- Only after both platforms pass, a unique prerelease is created containing
  `jnitrace.jar` and `jnitrace-source.zip`. Each prerelease uses a new tag;
  previous releases are retained. Pull requests build/test without publishing.
- The release job uses the built-in `GITHUB_TOKEN` with `contents: write`.
  Repository/organization policies must allow this permission.

CI uses the Maven compiler plugin explicitly (release 11) so a modern JDK does not
fall back to Maven's old source/target defaults. The Windows DLL statically links
the MinGW runtimes to avoid requiring separate MinGW runtime DLLs.

## Local build (optional)

On Ubuntu with JDK 21, Maven, g++, and `g++-mingw-w64-x86-64-posix` installed:

```bash
bash scripts/build.sh
```

Output: `target/jnitrace.jar`. Build native libraries **before** packaging Java;
the JNI initialization signature changed, so old DLL/SO files are incompatible.
