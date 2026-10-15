import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

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
with tempfile.TemporaryDirectory() as tmp:
    for compact in (0, 1):
        out = pathlib.Path(tmp) / str(compact)
        options = f"module=fixture,include=obf.*;native.*,out={out},compact={compact}"
        subprocess.run(["java", f"-javaagent:{jar}={options}", "-cp", str(root / "target/test-classes"), "obf.Fixture", str(lib)], check=True)
        trace = out / "jni_trace.jsonl"
        sizes[compact] = trace.stat().st_size
        raw = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        records = list(module.expand(raw))
        assert raw, "No trace produced (check native initialization stderr)"
        enters = {x["seq"]: x for x in records if x["ev"] == "enter"}
        exits = {x["seq"]: x for x in records if x["ev"] == "exit"}
        assert len(enters) == 3200, len(enters)
        assert enters.keys() == exits.keys()
        assert len(records) == 2 * len(enters)
        assert len({x["tid"] for x in enters.values()}) == 2
        assert all(x["in"].startswith("obf.Fixture.") for x in enters.values())
        assert any(x.get("val") == "A\x00中😀" for x in exits.values())
        assert sum(bool(x.get("exception")) for x in exits.values()) == 400
        assert not (out / "jni_trace.json").exists()
        assert any(x.get("ev") == "context" for x in raw) == bool(compact)
    # Filter rejection should create an empty trace.
    out = pathlib.Path(tmp) / "reject"
    subprocess.run(["java", f"-javaagent:{jar}=module=fixture,include=does.not.match.*,out={out}", "-cp", str(root / "target/test-classes"), "obf.Fixture", str(lib)], check=True)
    assert (out / "jni_trace.jsonl").read_bytes() == b""
assert sizes[1] < sizes[0], sizes
print(f"Smoke passed; plain={sizes[0]} compact={sizes[1]} bytes")
