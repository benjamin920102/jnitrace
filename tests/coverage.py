"""Run the same semantic fixture untraced and with both trace encodings."""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
root=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('expand',root/'scripts/expand_trace.py')
expand=importlib.util.module_from_spec(spec);spec.loader.exec_module(expand)
subprocess.run(['javac','-d',str(root/'target/test-classes'),str(root/'tests/Probe.java')],check=True)
lib=(root/'target'/('probe.dll' if sys.platform=='win32' else 'libprobe.so')).resolve()
base=['-cp',str(root/'target/test-classes'),'obf.Probe',str(lib)]
baseline=subprocess.run(['java','-Xcheck:jni',*base],check=True,capture_output=True,text=True)
assert baseline.stdout.strip()=='PROBE_OK',baseline.stdout
with tempfile.TemporaryDirectory() as tmp:
    for compact in (0,1):
        out=pathlib.Path(tmp)/str(compact)
        proc=subprocess.run(['java','-Xcheck:jni',f'-javaagent:{root}/target/jnitrace.jar=module=probe,include=obf.*,out={out},compact={compact}',*base],check=True,capture_output=True,text=True)
        assert proc.stdout==baseline.stdout,proc.stdout
        assert 'native entry/return: enabled' in proc.stderr,proc.stderr
        records=list(expand.expand([json.loads(x) for x in (out/'jni_trace.jsonl').read_text().splitlines()]))
        enter={x['seq']:x for x in records if x['ev']=='enter'}
        leave={x['seq']:x for x in records if x['ev']=='exit'}
        assert enter.keys()==leave.keys()
        assert len(enter)==len(records)//2
        native=[x for x in enter.values() if x['fn']=='NativeMethod']
        assert len({x['tid'] for x in native})==3 # install on main, two workers
        for x in enter.values():
            assert int(x['va'],16)-int(x['rva'],16)==int(x['base'],16)
            if 'native_parent' in x: assert enter[x['native_parent']]['fn']=='NativeMethod'
        mixed=[x for x in native if x['name']=='mixed'];assert len(mixed)==10
        for x in mixed:
            assert x['args']==[True,-7,20013,-300,1234,9000000000,1.25,2.5,'A\x00中😀',9,4.5,8000000000,2.25,-17,8.5],x
            assert leave[x['seq']]['val']==101.25
        child=[x for x in native if x['name']=='child'];assert len(child)==10
        assert all(x['depth']==1 and x['native_parent'] for x in child)
        expected={'retBoolean':True,'retByte':-101,'retChar':20013,'retShort':-30123,'retInt':-123456789,'retFloat':1.25,'retDouble':-3.5,'instance':9999999998}
        for name,value in expected.items():
            calls=[x for x in native if x['name']==name];assert len(calls)==10
            assert all(leave[x['seq']]['val']==value for x in calls),(name,calls)
        assert all(leave[x['seq']]['exception'] and 'val' not in leave[x['seq']] for x in native if x['name']=='thrower')
        fns={x['fn'] for x in enter.values()}
        required={'RegisterNatives','NativeMethodBind','GetObjectClass','FindClass','GetMethodID','GetStaticMethodID','GetFieldID','GetStaticFieldID','NewObjectV','AllocObject','ExceptionOccurred','ExceptionClear','Throw','ThrowNew','GetStringCritical','ReleaseStringCritical','GetPrimitiveArrayCritical','ReleasePrimitiveArrayCritical','GetStringRegion','GetStringUTFRegion'}
        for t in ('Boolean','Byte','Char','Short','Int','Long','Float','Double'):
            required.update({f'New{t}Array',f'Get{t}ArrayElements',f'Release{t}ArrayElements',f'Get{t}ArrayRegion',f'Set{t}ArrayRegion'})
        assert required<=fns,required-fns
        assert any(leave[x['seq']].get('val')=='field' for x in enter.values() if x['fn']=='GetStaticObjectField')
        assert all(x.get('deferred') for x in enter.values() if 'Critical' in x['fn'])
        assert any(x.get('fieldID') and x.get('sig')=='I' and x.get('name')=='field' for x in enter.values())
        for x in enter.values():
            if x['fn']=='RegisterNatives':
                for m in x['methods']:assert int(m['fn'],16)-int(m['rva'],16)==int(m['base'],16)
        print(f'Coverage compact={compact}: {len(enter)} paired calls, {len(native)} native invocations; ABI, values, nesting, exceptions, buffers, addresses passed')
