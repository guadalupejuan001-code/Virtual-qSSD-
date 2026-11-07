# C05 — traslado Bell transaccional de un byte cuántico

La teletransportación simulada **ya existía** en C01: `bell_transfer` sobre
`QuantumRuntime` XI/Ether y `Space::teleport_qubit` para **un solo qubit**.
El bus `Space::route` ya compartía una identidad lógica entre CPU/GPU/SSD,
sin ejecutar ningún circuito Bell. No son el mismo mecanismo.

## Extensión compatible

`Space::teleport_quantum_byte(port, source_qbase, auxiliary_qbase, destination_qbase)`
realiza ocho teletransportaciones Bell consecutivas en el runtime original
para un byte *explícitamente* vinculado a ocho qubits en InverseMemory.
Emite un `std::array<BellReceipt,8>` con las 16 mediciones clásicas.
Exige que la vinculación original de InverseMemory coincida exactamente con
`source_qbase`, y que los 24 qubits de origen/auxiliares/destino no se
solapen. Los auxiliares y destinos deben estar en |0>; esa restricción ya
es verificada por el protocolo original. Una excepción, incluida una
excepción después del primer Bell, **no publica ninguna actualización**.

Una transacción retargetea el byte lógico de InverseMemory al nuevo registro
de ocho qubits y publica otra revisión de la misma identidad QSSD. Los
puertos CPU, GPU y SSD previamente unidos a esa identidad reciben el
mismo registro al adquirir una vista nueva. Los lectores que retienen una
vista antigua conservan su revisión y sus resultados antiguos.

No copia datos densos para `route`, no reemplaza Compact Core, ni cambia
los formatos RVPC2 / XQSSD1 / QSB3. No reinterpreta una dirección compartida
como entrelazamiento físico. Para la representación actual de InverseMemory,
la unidad explícita seleccionada es un byte de ocho qubits; no significa
que cualquier memoria clásica se vuelva cuántica ni que el bus físico
de CPU/GPU deje de transmitir datos.

## Comprobaciones

`qssd_atomic_bell_byte` ejecuta circuitos entrelazados no Clifford (H, T,
CX) para ocho semillas; compara correlaciones Pauli individuales, de dos
qubits y de tres qubits antes (registro fuente) y después (destino).
También valida las cuatro ramas Bell, las vistas retenidas, los alias
CPU/GPU/SSD, checkpoint/restauración y rollback completo ante superposición
de rangos, origen equivocado y un auxiliar/destino no preparado.

El campo `host_mean_handoff_us` es **tiempo de ejecución de software
en la CPU del runner de CI**, no rendimiento de GPU física ni latencia de
un canal cuántico real. Las métricas de C01 `qssd_space measure` siguen
midiendo entregas de referencias lógicas, no una transferencia Bell,
ni una ventaja validada sobre copias densas en todos los casos.

## Alcance pendiente

- Ejecutar pruebas independientes con estados más generales y tolerancias
  cuantificadas; la cobertura finita no constituye demostración universal.
- Comparar requisitos y costos del contrato de entrega frente a una copia
  convencional de igual semántica, en el mismo hardware.
- Añadir adaptadores explícitos para GPU física / Android y medir costes
  reales de sincronización y materialización.

C04 sobre crecimiento dinámico sigue siendo independiente y se conserva.
