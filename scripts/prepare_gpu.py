"""Verified read-only sharing accessor; the pinned GPU checkout stays intact."""
import hashlib
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
expected = {
    'estado_gpu.hpp': '91ed5bd74a3c6ed125a86436feb08994b56cfa30cadfe35822b21d1698775174',
    'programa.hpp': 'aea125a017c906d77f140743f4ded7912bb973bd845dd317b666574e6c8d27e8',
}
output.mkdir(parents=True, exist_ok=True)
for name, digest in expected.items():
    raw = (source / name).read_bytes()
    if hashlib.sha256(raw).hexdigest() != digest:
        raise SystemExit('Unreviewed GPU interface: ' + name)
    text = raw.decode()
    if name == 'estado_gpu.hpp':
        anchor = '    bool has_output() const { return static_cast<bool>(output_); }'
        if text.count(anchor) != 1:
            raise SystemExit('Ambiguous GPU interface extension')
        text = text.replace(anchor,
            '    std::shared_ptr<const Field> output_view() const {\n'
            '        require(static_cast<bool>(output_), "sin salida");\n'
            '        return output_;\n'
            '    }\n' + anchor)
    path = output / name
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
