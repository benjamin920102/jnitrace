import importlib.util
import json
import pathlib
import subprocess
import sys
import contextlib

root = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("expand_trace", root / "scripts/expand_trace.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
# Validate malformed trace rejection as well as context expansion.
try:
    list(module.expand([{"ev": "enter", "ctx": 999}]))
except ValueError:
    pass
else:
    raise AssertionError("Expander must reject unknown contexts")
jar = root / "target/jnitrace.jar"
lib = (root / "target" / ("fixture.dll" if sys.platform == "win32" else "libfixture.so")).resolve()
subprocess.run(["javac", "-d", str(root / "target/test-classes"), str(root / "tests/Fixture.java")], check=True)
sizes = {}
# Keep traces and JVM crash reports when the subprocess dies. A temporary
# directory would delete the only evidence before Actions can upload it.
trace_root = root / "target/smoke-traces"
trace_root.mkdir(parents=True, exist_ok=True)

def run_java(command):
    report = trace_root / "hs_err_pid%p.log"
    command = [command[0], f"-XX:ErrorFile={report}", *command[1:]]
    result = subprocess.run(command, check=False, capture_output=True, text=True, errors="replace")
    (trace_root / "java-stdout.log").write_text(result.stdout, encoding="utf-8")
    (trace_root / "java-stderr.log").write_text(result.stderr, encoding="utf-8")
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    if result.returncode:
        code = result.returncode & 0xffffffff
        raise RuntimeError(f"Java exited with {result.returncode} (0x{code:08X}); diagnostics: {trace_root}")

with contextlib.nullcontext(str(trace_root)) as tmp:
    for compact in (0, 1):
        out = pathlib.Path(tmp) / str(compact)
        options = f"module=fixture,include=obf.*;native.*,out={out},compact={compact},flush=1"
        run_java(["java", f"-javaagent:{jar}={options}", "-cp", str(root / "target/test-classes"), "obf.Fixture", str(lib)])
        trace = out / "jni_trace.jsonl"
        sizes[compact] = trace.stat().st_size
        raw = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        records = list(module.expand(raw))
        assert raw, "No trace produced (check native initialization stderr)"
        enters = {x["seq"]: x for x in records if x["ev"] == "enter"}
        exits = {x["seq"]: x for x in records if x["ev"] == "exit"}
        assert len(enters) == 5600, len(enters)
        assert enters.keys() == exits.keys()
        assert all(x.get("schema") == 2 for x in enters.values())
        assert len({x["tid"] for x in enters.values()}) == 2
        assert all(x["in"].startswith("obf.Fixture.") for x in enters.values())
        assert any(isinstance(x.get("val"), dict) and x["val"].get("text") == "A\x00中😀" for x in exits.values())
        assert sum(bool(x.get("exception")) for x in exits.values()) == 400
        assert not (out / "jni_trace.json").exists()
        assert any(x.get("ev") == "context" for x in raw) == bool(compact)
    # Filter rejection should produce no JNI operations (session metadata remains).
    out = pathlib.Path(tmp) / "reject"
    run_java(["java", f"-javaagent:{jar}=module=fixture,include=does.not.match.*,out={out},flush=1", "-cp", str(root / "target/test-classes"), "obf.Fixture", str(lib)])
    assert not any(x.get("ev") == "enter" for x in map(json.loads, (out / "jni_trace.jsonl").read_text().splitlines()))
assert sizes[1] < sizes[0], sizes
print(f"Smoke passed; plain={sizes[0]} compact={sizes[1]} bytes")
