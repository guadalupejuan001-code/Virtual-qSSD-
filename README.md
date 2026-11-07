# Virtual qSSD — espacio de estados compartido

**C06 — Tres núcleos en el mismo laboratorio:** qSSD fija GPU-VIRTUAL C12
(base oficial), la CPU XI/Ether original de REVERSE-PC y el estado qSSD C05.
Una interfaz validada permite a C12 consumir el **mismo campo inmutable** que
publicó qSSD sin copiar el dominio. La CPU publica coeficientes por XI, C12
calcula relaciones y binary32, y QSSD mantiene las revisiones/checkpoints.
Las vistas cuánticas conservan estados y correlaciones del XI original;
sus observaciones se ejecutan en CPU anfitriona, no en GPU física.
[Pruebas y límites C06](docs/C06-three-cores-experiment.md).

**C05:** la teletransportación Bell simulada original de un qubit sigue
intacta. Se añade `teleport_quantum_byte` para trasladar atómicamente los ocho
qubits de una vinculación explícita de InverseMemory y publicar el mismo objeto
QSSD a través de sus puertos CPU/GPU/SSD, con 16 mediciones clásicas, validación
de origen, rollback y comprobación Pauli antes/después. Una prueba host compara
entregas de referencias con copias densas, sin equiparar las semánticas ni
atribuir el resultado a hardware cuántico. [Alcance C05](docs/C05-quantum-byte-handoff.md).

**C03:** `qssd/state_bus.hpp` añade una autoridad para estados lógicos completos,
transacciones, conflictos, historial, marcadores, restauración de versiones y
bifurcaciones independientes. `qssd/state_archive.hpp` conserva checkpoints
deduplicados verificables. La integración de Velocity almacena la CPU nativa,
sus recursos y programas, componentes y el trabajo gráfico con sus dependencias.
Véase [el diseño y alcance de C03](docs/C03-state-bus.md).

**C02:** `qssd/shared_projection.hpp` exporta observaciones inmutables de una
autoridad existente a puertos CPU/SSD/GPU. Una entrega conserva la identidad y
la revisión; un resultado idéntico conserva el mismo objeto. Los consumidores
pueden materializar una representación para su API física, contabilizando esa
proyección por separado. La integración de REVERSE Velocity conserva sus
ecuaciones y primitivas XI/REgraphics.

Componente propio de X construido con las reglas y los núcleos de **Mundo X /
Ether, REVERSE-PC y GPU-VIRTUAL**. Su primera versión C01 conecta la CPU XI,
el perfil nativo GPU C01 y qSSD mediante una autoridad común. El contenido se
conserva como reglas, regiones, relaciones y diferencias exactas.

El cambio central es que un dato tiene una identidad y una revisión. Los puertos
`cpu:`, `gpu:` y `ssd:` resuelven esa misma identidad. Transferir su disponibilidad
consiste en publicar una relación; transformar el contenido publica un nuevo
estado compacto de esa identidad. No se construye una copia completa del dominio
para cada componente.

## Conexión que ejecuta C01

| Parte | Código original utilizado | Integración en este repositorio |
| --- | --- | --- |
| CPU y estado cuántico | `QuantumRuntime`, XI e InverseMemory de Mundo X, extensiones verificadas de REVERSE-PC | Una instancia del runtime dentro de la autoridad común; ejecución de programas XI y vistas cuánticas originales. |
| GPU virtual nativa | `xgpu::Machine`, `Field`, `Mode` de GPU-VIRTUAL | Sus campos inmutables se comparten por referencia; sus operaciones transforman reglas y detalles uint32. |
| qSSD | `qssd::Space` | Identidades, puertos, revisiones, entrega de referencias y checkpoints del estado conjunto. |

La demostración ejecuta realmente los dos núcleos originales:

1. XI calcula coeficientes y los conserva en su InverseMemory.
2. qSSD obtiene ocho bytes de descripción para una regla de dos coeficientes.
3. El productor GPU original calcula un campo con regiones y un detalle exacto.
4. El campo conserva **la misma dirección de objeto C++** al publicarse en qSSD.
5. El motor `Field` original combina las dos descripciones. CPU, GPU y qSSD
   observan la misma identidad/revisión del resultado.
6. Un cambio publicado desde el puerto CPU queda visible desde GPU y qSSD.
7. Una observación explícita proyecta cuatro bytes y los carga con instrucciones
   reales de XI. El dominio completo sigue sin materializarse.
8. Vistas cuánticas exponen los handles de InverseMemory. Un traslado de Bell
   independiente conserva correlaciones entre sitios del registro cuántico.

Los núcleos se fijan por SHA en `dependencies.json`; no se reescriben ni se
incorporan motores ajenos. El prototipo anterior de bloques QSSD1 de REVERSE-PC
no es una dependencia de este espacio.

`prepare_gpu.py` genera una interfaz local con un único accesor de lectura
`output_view()`. Comprueba SHA-256 de los encabezados originales y permite
compartir su `shared_ptr<const Field>` sin retener todo el ejecutor GPU. Las
pruebas originales de GPU-VIRTUAL se ejecutan también con esa interfaz. La
dependencia fijada permanece intacta.

## Operaciones del espacio

| Operación | Cambio que realiza |
| --- | --- |
| `publish_cpu_rule` | Obtiene solo los coeficientes solicitados del estado real de la CPU. |
| `publish_gpu` | Conserva una referencia al campo inmutable original, sin copiar ni retener el programa u otros registros del productor. |
| `bind` / `route` | Conecta un puerto con una identidad; una entrega puede consumir el enlace de origen. |
| `gpu_binary` | Suma o multiplica reglas, regiones y detalles con el perfil exacto uint32 existente. |
| `patch` | Publica una diferencia exacta manteniendo la identidad. |
| `acquire` | Retiene una vista de una revisión para trabajo en curso. |
| `observe` / `cpu_load_word` | Produce únicamente el valor solicitado. La segunda operación proyecta cuatro bytes a un buzón explícito de la CPU. |
| `publish_quantum_byte` | Crea una vista sobre ocho direcciones reales del registro, mediante el `mem_bind` original. |
| `teleport_qubit` | Ejecuta H/CX, mediciones Born y correcciones X/Z en el runtime original. Consume el estado de origen y comunica dos resultados clásicos dentro del simulador. |
| `checkpoint` / `restore` | Guarda y recupera la CPU/RNG/memoria, los campos compactos únicos, las identidades y los enlaces. |

Un enlace lógico compartido **no se contabiliza como entrelazamiento cuántico ni
como un traslado de Bell**. Los dos mecanismos tienen APIs y pruebas diferentes.
Un handle cuántico no se convierte en una palabra uint32 ni se mide
automáticamente para alimentar el perfil aritmético de GPU C01.

## Reproducir

```sh
python3 scripts/setup.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/qssd_space demo primer-estado.xqs
./build/qssd_space inspect primer-estado.xqs
./build/qssd_space observe primer-estado.xqs gpu:joint 42
./build/qssd_space measure
```

Continuar XI o llevar su estado a la CPU existente:

```sh
./build/qssd_space run-xi primer-estado.xqs continuar.xi continuado.xqs
./build/qssd_space export-cpu continuado.xqs estado-nativo.rvp
```

`estado-nativo.rvp` usa RVPC2 sin modificar su formato. `Space` copiado en memoria
conecta otro cliente a la misma autoridad; `restore` crea una autoridad
independiente desde un checkpoint explícito. Una vista retenida conserva su
revisión; adquirir de nuevo el puerto observa la revisión actual. La clave de
una caché debe incluir la revisión de la autoridad para las vistas cuánticas.

## Evidencia y alcance

Las pruebas comparan el recorrido CPU → reglas → GPU → qSSD → CPU con valores
densos calculados de forma independiente. Verifican cambios y versiones
compartidos, vistas antiguas, ocho escritores concurrentes, corrupción,
rechazos sin publicación parcial y persistencia de la topología de enlaces.
También prueban 32 traslados de un sitio de pares entrelazados no Clifford y
un estado GHZ de 1000 cúbits, junto a un campo de 2^32 identidades. Trasladar un
sitio de ese GHZ requiere dos auxiliares adicionales, contabilizados por separado.

`measure` realiza 10000 entregas de referencia por dominio, para 1024,
1048576 y 4294967296 palabras lógicas. Comprueba que conserva el campo nativo y
no solicita valores densos durante las entregas. Informa también tiempo del
anfitrión; los cambios de estado del modelo requieren trabajo físico para
ejecutarse. No compara contra un SSD físico ni acredita mejoras de FPS.

El perfil probado es **integración nativa sobre CPU anfitriona**. Los shaders
C09/C10 y el renderizador Android todavía no consumen estos puertos. No se ha
medido esta integración en GPU física ni en el Galaxy A16. Conectarlos requiere
un adaptador explícito con sus formatos y sincronización, conservando los
resultados de esos perfiles.

El dominio de `Field` conserva el límite original de 2^32 identidades y uint32
módulo 2^32. Las direcciones de InverseMemory conservan sus presupuestos
originales; aquí no hay reserva densa de ese espacio. qSSD admite 1024 objetos,
4096 puertos, hasta 12 MiB estimados de descripciones compactas y checkpoints
de hasta 64 MiB. El tamaño de una descripción o checkpoint no es la RAM del
proceso ni capacidad física nueva. Datos arbitrarios pueden necesitar muchos
detalles y alcanzar sus presupuestos.

Los cambios se confirman con una raíz inmutable y un bloqueo de escritores;
se copian metadatos compactos según haga falta. El disco anfitrión se utiliza
solamente al pedir un checkpoint. El archivo `XQSSD1` preserva RVPC2 y deduplica
campos compartidos; su checksum FNV detecta corrupción accidental, no ataques.
La publicación de checkpoints utiliza un temporal, `fsync` y un enlace atómico
a un nombre nuevo; una falla posterior a la publicación informa incertidumbre
de durabilidad. No hay controlador NVMe/virtio ni montaje automático en Android.
