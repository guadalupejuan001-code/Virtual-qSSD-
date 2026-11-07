# C06 — CPU XI/Ether + GPU-VIRTUAL C12 + QSSD C05

## Por qué existe

El repositorio qSSD seguía consumiendo el GPU C01 fijado en
`dependencies.json`; el repositorio GPU-VIRTUAL había promovido C12 a la
base oficial. El CPU XI principal de REVERSE-PC (commit
`08c363e6c6855c14026c86046af7183a45f45a98`) ya estaba fijado en
qSSD. El núcleo cuántico original de Mundo X es idéntico entre el
checkout anterior `e2778d9` y `main` `6f36ba5` en las rutas
`infinite_core.hpp`, `quantum_runtime.hpp`,
`inverse_memory.hpp`, `exact_t_experiment.hpp` y `ether_core.hpp`.
Las revisiones posteriores afectan otras piezas, como X Render, y su
compatibilidad con la extensión versionada de REVERSE-PC debe comprobarse.
La prueba de actualizar **todo** Mundo X a `6f36ba5` fue rechazada por
el verificador de ABI: `ether_render_spatial.hpp` cambió respecto al perfil
que REVERSE-PC fija. C06 conserva **intencionadamente** el SHA anterior
`e2778d9`, cuyos núcleos de CPU/quantum son idénticos a los de `main`,
para no modificar REgraphics ni desactivar verificaciones de integridad.
La CPU virtual sigue utilizando el `main` vigente de REVERSE-PC.

C06 fija y conserva el núcleo C12 oficial `4cdd47f`. No cambia el código
original de los núcleos. `scripts/prepare_c12.py` valida por hash Git blob
los encabezados oficiales y genera un adaptador local de compilación:
`Value::polynomial(std::shared_ptr<const Field>)` y
`Execution::initialize_global_shared(std::shared_ptr<const Field>)`.

Estos puntos permiten que el ejecutor C12 reciba **el mismo objeto
inmutable** que QSSD publicó. No materializan el dominio, no ejecutan
un circuito Bell para cada acceso y no copian los coeficientes ni sus
detalles al aceptar una revisión. El programa C12 real sí consume
tiempo del procesador anfitrión y produce nuevas relaciones de salida.

## Tres operaciones verificadas

1. **CPU -> QSSD -> GPU C12**: XI ejecuta un programa de CPU y escribe
   coeficientes en InverseMemory. QSSD forma un campo compacto, lo
   publica y entrega a C12 la misma referencia. C12 ejecuta instrucciones
   `LoadGlobal`, suma y `FloatFromUnsigned/FloatAdd` con el perfil
   C12 real, sin usar un ejecutor C01 como sustituto.
2. **Lectura coherente por versión**: cuando XI/QSSD publica un cambio,
   nuevos clientes C12 adquieren el nuevo campo; una ejecución C12 que
   ya estaba en marcha conserva la versión original. Tras guardar y
   restaurar XQSSD1, el mismo programa se reproduce sobre el estado
   recuperado.
3. **Estado cuántico compartido y traslado Bell**: un puerto lógico GPU
   puede retener una vista del estado original de XI/Ether y consultar
   correlaciones Pauli **mediante el núcleo XI anfitrión**, sin copiar
   un vector de amplitudes. C05 puede trasladar, de manera simulada,
   los ocho qubits asociados a un byte; el puerto readquirido observa
   el estado reconstruido y los lectores antiguos conservan su revisión.

## Latencia y alcance

`qssd_c12_tests` compara dos operaciones de **igual semántica**:
inicializar el C12 desde una copia del pequeño descriptor compacto o
inicializarlo por la referencia original. La prueba registra medias
`shared_initialization_mean_us` y
`descriptor_copy_initialization_mean_us` sobre 256 muestras.
El procesador anfitrión, el ruido de planificador y el coste de
construir el ejecutor influyen en esos tiempos. Un resultado aislado
no acredita ventaja universal.

La teletransportación Bell y la entrega directa de un `shared_ptr`
**no son el mismo trabajo**, por lo que sus latencias no se deben
comparar como si fueran equivalentes. El segundo solo comparte
representaciones en el **mismo espacio de memoria**. El primero
transforma el estado lógico simulado usando H/CX, dos bits clásicos
por qubit y correcciones X/Z; cuesta operaciones host.

**No demostrado**: acceso por hardware cuántico físico, transmisión
instantánea, ejecución de puertas cuánticas por C12, transporte sin
movimiento de bits entre procesadores físicos, aumento de FPS de un
juego real. C12 conserva sus límites implementados de nodos, pasos,
memoria y formatos sin promesas de capacidad ilimitada.

## Reproducir

```sh
python3 scripts/setup.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/qssd_c12_tests
```

La CI conserva los controles anteriores y las verificaciones de C12.
C04 (crecimiento dinámico) es una PR distinta y no se sustituye.
