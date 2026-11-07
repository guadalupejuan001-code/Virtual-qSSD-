#!/usr/bin/env python3
"""Pin-checked, read-only compatibility extension for the official C12 GPU.

The original GPU-VIRTUAL checkout stays untouched. It already supports
immutable C01 fields; we only expose a shared-pointer entry point to the
existing C12 executor to avoid copying a compact field on handoff.
"""
import hashlib
from pathlib import Path
import sys

source, output = (Path(value) for value in sys.argv[1:3])
expected_git_blobs = {
    "relaciones_gpu.hpp": "41b804411c4d9d796a1128997c30b3590b44e87d",
    "ejecucion_gpu.hpp": "17efe23517693b5b9b311d1397c2c654b9e4b3cb",
}
output.mkdir(parents=True, exist_ok=True)

def add_once(text, anchor, insertion, name):
    if text.count(anchor) != 1:
        raise SystemExit("Unreviewed C12 extension anchor: " + name)
    return text.replace(anchor, insertion + anchor, 1)

for name, blob in expected_git_blobs.items():
    raw = (source / name).read_bytes()
    git_blob_hash = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
    if git_blob_hash != blob:
        raise SystemExit("Unreviewed GPU C12 header: " + name)
    text = raw.decode("utf-8")
    if name == "relaciones_gpu.hpp":
        text = add_once(
            text, "    static FieldRoot node(ValueKind kind,Index n,FieldRoot left,FieldRoot right,Index immediate,LaneMask mask,",
            """    // QSSD bridge: preserve the exact immutable Field pointer, not just
    // an equal copy of its compact coefficients and patches.
    static FieldRoot polynomial(std::shared_ptr<const Field> source) {
        require(static_cast<bool>(source),"campo QSSD nulo");
        const auto n=source->count;
        return FieldRoot(new Value(ValueKind::Polynomial,n,std::move(source),{},{},0,LaneMask(n,true),{}));
    }
""", name + ":Value")
        text = add_once(
            text, "inline FieldRoot field_root(Field field) {return Value::polynomial(std::move(field));}",
            """inline FieldRoot field_root(std::shared_ptr<const Field> source) {
    return Value::polynomial(std::move(source));
}
""", name + ":field_root")
    else:
        text = add_once(
            text, "    std::size_t enqueue(Kernel kernel,Index n,unsigned local=64,unsigned shared_words=0,",
            """    // QSSD bridge: initialize C12 from the existing immutable C01 field.
    // It does not transfer a dense array or alter GPU-VIRTUAL's sources.
    void initialize_global_shared(std::shared_ptr<const Field> source) {
        require(static_cast<bool>(source) && jobs_.empty() && source->count==global_->count,
                "inicializacion QSSD fuera de fase");
        source->validate();
        auto next=field_root(std::move(source));
        require(revision_!=UINT64_MAX,"revision agotada");
        global_=std::move(next); ++revision_;
    }
""", name + ":Execution")
    path = output / name
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
print("C12 immutable shared-field ABI verified")
