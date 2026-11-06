import base64
import json
from pathlib import Path
import resource
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())


def run(*args, ok=True):
    p = subprocess.run([binary, *map(str, args)], capture_output=True, text=True, timeout=60)
    assert (p.returncode == 0) == ok, (args, p.stdout, p.stderr)
    return p.stdout


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    legacy = root / 'c03.xqs'
    legacy.write_bytes(base64.b64decode((Path(__file__).parent / 'fixtures' / 'c03-demo.xqs.b64').read_bytes()))
    assert json.loads(run('inspect', legacy))['checkpoint_bytes'] == legacy.stat().st_size
    for port in ('cpu:joint', 'gpu:joint', 'ssd:joint'):
        assert json.loads(run('observe', legacy, port, 42))['value'] == 0x12345678
    legacy_program = root / 'c03-continue.xi'
    legacy_program.write_text('expect Z:400 Z:200\nmem_store 123 77\n')
    legacy_continued = root / 'c03-continued.xqs'
    assert json.loads(run('run-xi', legacy, legacy_program, legacy_continued))['value'] == 1
    legacy_native = root / 'c03-native.rvp'
    run('export-cpu', legacy_continued, legacy_native)
    assert legacy_native.read_bytes()[:5] == b'RVPC2'
    checkpoint = root / 'space.xqs'
    result = [json.loads(line) for line in run('demo', checkpoint).splitlines()]
    assert result[0]['same_native_gpu_field'] and result[0]['cpu_gpu_ssd_same_identity_and_revision']
    assert result[0]['routing_dense_words_materialized'] == 0
    assert result[0]['explicit_cpu_projection_bytes'] == 4
    assert result[0]['cpu_output']['value'] == 0x12345678
    assert result[0]['quantum_handle_preserved'] and result[0]['bell_preserved_partner_ZZ'] == 1
    before = checkpoint.read_bytes()
    info = json.loads(run('inspect', checkpoint))
    assert info['checkpoint_bytes'] == len(before) and info['logical_field_words'] >= 1 << 32
    for port in ('cpu:joint', 'gpu:joint', 'ssd:joint'):
        assert json.loads(run('observe', checkpoint, port, 42))['value'] == 0x12345678
    run('observe', checkpoint, 'gpu:q', 0, ok=False)
    run('observe', checkpoint, 'gpu:missing', 0, ok=False)
    run('demo', checkpoint, ok=False)
    assert checkpoint.read_bytes() == before
    program = root / 'continue.xi'
    program.write_text('expect Z:400 Z:200\nmem_store 123 77\n')
    continued = root / 'continued.xqs'
    assert json.loads(run('run-xi', checkpoint, program, continued))['value'] == 1
    native = root / 'native.rvp'
    run('export-cpu', continued, native)
    assert native.read_bytes()[:5] == b'RVPC2'
    program.write_text('x 9\ninvalid_opcode\n')
    absent = root / 'failed.xqs'
    run('run-xi', checkpoint, program, absent, ok=False)
    assert not absent.exists() and checkpoint.read_bytes() == before
    bad = root / 'bad.xqs'; damaged = bytearray(before); damaged[len(damaged)//2] ^= 0x55; bad.write_bytes(damaged)
    run('inspect', bad, ok=False)
    assert bad.read_bytes() == damaged

    def limit_write():
        resource.setrlimit(resource.RLIMIT_FSIZE, (128, 128))

    p = subprocess.run([binary, 'demo', str(absent)], capture_output=True, timeout=60, preexec_fn=limit_write)
    assert p.returncode != 0 and not absent.exists() and checkpoint.read_bytes() == before

print(json.dumps({'checkpoint_cli': 'PASS', 'mixed_native_state': True,
                  'same_three_port_values': True, 'native_rvpc2_export': True,
                  'partial_checkpoint_preserved_previous': True, 'c03_checkpoint_compatible': True}))
