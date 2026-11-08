# C07: QSSD is the component, not the repository or an application-side cache

Continues exact C06 9fe72bbf0d972d0baad9300971dfaabab5691447. Original CPU/XI,
GPU-VIRTUAL C12, Bell, shared projection and archive APIs are retained.

The new GraphicsPort belongs to THIS component and uses its original StateBus
for coherent immutable graphics-resource revisions. It imports the complete
session/identity/revision of an existing native geometry lease. It does not
rebuild or claim ownership of the producer's world simulation. Palettes, atlas
layout, partial transfer decisions and device acceptance receipts are managed
natively here, not by a second Java resource manager in a consuming game.

An unchanged atlas is not allocated or reorganized when only geometry moves.
An unchanged palette creates no 512-entry scratch array. A failed or partial
upload invalidates the physical replica; retry is full. Stale acknowledgments,
foreign component views and obsolete sessions are rejected. A confirmation
means the host graphics API accepted the update, NOT that the physical GPU
finished rendering it. Logical state and hardware residency are distinct.

The additive Space::publish_field API imports an explicitly classical compact
producer descriptor into QSSD. Existing bind/route/acquire and C12Window then
share its exact immutable pointer across CPU/SSD/GPU ports. No fake quantum
transfer, implicit measurement, new executor, or dense expansion is introduced.
C12's original interpreter still executes on the host CPU.

New tests exercise sparse updates, concurrent immutable leases, failed-transfer
recovery, source versions, shared C12 pointers and original checkpoint recovery.
Existing QSSD tests are retained. Performance improvement requires separate
same-scene/device measurements; none is claimed by this source integration.
