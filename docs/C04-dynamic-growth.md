# C04 — crecimiento dinámico de QSSD

QSSD conserva las revisiones por defecto y asigna recursos conforme llegan los
datos. Se retiran las cuotas de la capa de almacenamiento que impedían continuar
al alcanzar un número fijo de entradas o bytes.

| Componente | Comportamiento actual |
| --- | --- |
| Historial de `StateBus` | Crece bajo demanda; no descarta revisiones por defecto. |
| Marcadores | Crecen bajo demanda. |
| Objetos y puertos de `Space` | Crecen bajo demanda, conservando identidad y referencias compartidas. |
| Descripciones compactas agregadas | Sin cuota adicional de qSSD. |
| Checkpoints `XQSSD1` y `QSB3` | Sin cuota propia de bytes o secciones; se comprueban las dimensiones reales. |
| Lectura de archivos y programas XI | Sin cuota predeterminada de qSSD; la API permite una capacidad solicitada explícitamente. |

## Retención e integridad

`StateBus(initial)` equivale a `StateBus(initial, 0)`: el cero expresa ausencia de
expulsión automática. La colección se amplía al publicar cada revisión; no
preasigna memoria según una capacidad máxima. Una capacidad positiva conserva
la opción de retención finita elegida por el consumidor, sin el antiguo máximo
de 4096. `stats().historyCapacity` devuelve cero en el modo predeterminado. Una
bifurcación hereda esa política y sigue compartiendo los datos inmutables de su
punto de partida.

Las búsquedas de revisiones usan orden por número de revisión. Las asignaciones
que pueden fallar se realizan antes de publicar la nueva autoridad: un error
conserva el estado anterior. Los lectores en curso retienen su revisión, incluso
cuando un consumidor ha solicitado una retención finita.

Los decodificadores verifican el checksum, las referencias, las dimensiones y
los bytes disponibles antes de recorrer las tablas. Los recuentos falsificados
no autorizan reservar memoria según un supuesto número de entradas. La
deduplicación sigue comparando los bytes completos y reconstruye las referencias
aliadas. Los formatos de archivo se conservan.

Las constantes públicas antiguas `max_objects`, `max_ports`,
`max_descriptor_bytes`, `max_checkpoint_bytes`, `StateArchive::max_bytes` y
`StateArchive::max_sections` se conservan por compatibilidad de código fuente y
expresan la representación de tamaños del anfitrión. No se sustituyen las cuotas
anteriores por otro número fijo elegido para el producto.

## Recursos y núcleos originales

El historial retenido ocupa memoria real y los checkpoints explícitos ocupan
disco. El crecimiento termina si el anfitrión no puede asignar más recursos o si
se agota la representación del formato; qSSD devuelve el error correspondiente.
Una colección creciente no se presenta como capacidad física infinita.

Se mantienen los núcleos y SHA de `dependencies.json`, incluidos sus formatos,
dominios y presupuestos internos. C04 cambia la capa QSSD y conserva fases,
correlaciones, RNG, campos compactos y proyecciones explícitas.

## Verificación reproducible

La suite `qssd_dynamic_growth` conserva más de 10000 revisiones, más de 1000
marcadores y una bifurcación de 5001 revisiones. Comprueba una retención solicitada
de 5000, vistas antiguas, rechazo sin publicación, archivos con más de 512
secciones y un contenido de 67108865 bytes que también se guarda y lee en disco.
Verifica referencias deduplicadas y recuentos imposibles con checksums válidos.

La suite de autoridad compartida publica 1101 objetos y 4201 puertos, restaura
el mismo checkpoint y verifica la topología de alias. Continúan las pruebas
originales de concurrencia, CPU XI, GPU, estados cuánticos y continuación del RNG.
La prueba CLI también restaura y continúa un checkpoint generado por C03 sin
modificar, conservado con su SHA-256 en `tests/fixtures`.

```sh
python3 scripts/setup.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Resultados locales: seis suites Release y seis suites con ASan/UBSan superadas.
La detección de fugas local no pudo ejecutarse porque el entorno impide a
LeakSanitizer consultar los directorios de tareas de `/proc`; el workflow de
GitHub conserva la detección de fugas activada. Los resultados y métricas locales
están en [evidence/c04/validation.json](../evidence/c04/validation.json).
