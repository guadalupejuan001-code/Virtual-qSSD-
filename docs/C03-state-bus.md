# C03 — estados completos y bus transaccional

`qssd/state_bus.hpp` conserva versiones inmutables de un estado lógico completo,
incluidos los núcleos originales y las referencias a sus recursos. La estructura
concreta del estado pertenece al consumidor: C03 no reemplaza las ecuaciones de
XI, los campos de GPU-VIRTUAL ni su aritmética.

Las copias de `StateBus<State>` abren clientes de una misma autoridad. Una lectura
retiene una raíz completa. `update` publica una transición validada; `publish`
también permite exigir una raíz de origen exacta y rechazar conflictos. Las
excepciones de la operación o del validador conservan la autoridad anterior.

Desde C04, el historial crece bajo demanda y conserva todas las revisiones por
defecto; los marcadores también crecen sin una cuota propia de qSSD. Una
retención finita solo se activa si el consumidor la solicita. `rewind` publica el
estado anterior con una revisión nueva; los lectores que trabajan con una vista
antigua siguen conservándola. `fork` crea otra autoridad que comparte los datos
inmutables iniciales y puede evolucionar por separado. Las referencias externas
también prolongan la vida de los datos; estos límites no son un límite de RAM
total del proceso.

`qssd/state_archive.hpp` proporciona el formato `QSB3` para checkpoints explícitos
sin las anteriores cuotas de 64 MiB y 512 secciones. Valida las dimensiones contra
los bytes presentes en el archivo. Deduplica contenidos por igualdad exacta de bytes
y recupera las secciones aliadas con el mismo objeto inmutable. Los hashes solo
aceleran la búsqueda y detectan corrupción accidental; no autentican contenido.
La ruta normal del bus no serializa ni escribe archivos.

## Integración real en REVERSE VELOCITY

La raíz de Velocity conserva:

- La instancia original de `QuantumRuntime`, registros, InverseMemory, estado
  cuántico y RNG, junto con salida, generación y último programa de CPU.
- Todos los recursos originales: mundo, conducción, renderizadores, shaders,
  atlas y paletas. CPU y cargador de shaders los consultan por el bus.
- El programa geométrico activo y el último trabajo gráfico preparado:
  geometría original XFG4, entradas de cámara/material, calidad y la CPU exacta,
  recursos y estados de componentes de los que dependió ese trabajo.
- Estados binarios de componentes, como selección de coche, controles, cambio,
  cámara suavizada, calidad gráfica y modo ambiental.

Una CPU nueva puede coexistir con un trabajo gráfico que depende de una CPU
anterior: ambas versiones se conservan y la dependencia es explícita. No se
etiqueta ese trabajo como si se hubiera calculado con el estado más reciente.

Guardar un recorrido serializa el estado nativo con RVPC2, recursos, programas,
componentes y trabajo gráfico. Recuperarlo valida todo el archivo antes de
aceptar otra autoridad. Los objetos del controlador gráfico se reconstruyen a
partir del estado lógico: números de objetos GL, colas del controlador, pantallas
del sistema y hardware físico no son estados portables de este archivo.

Las proyecciones JNI y las cargas a la GPU continúan siendo operaciones físicas
contabilizadas. C03 no establece una mejora de FPS por sí solo.

## Verificación

CTest ejecuta los controles anteriores y nuevas pruebas de 4000 escrituras
concurrentes, lectura coherente de CPU/GPU, conflictos, rechazo sin publicación,
historial, marcadores, bifurcaciones, restauración de archivos y corrupción.
Los núcleos originales verifican también un estado de 1000 cúbits correlacionados,
campos compactos compartidos y continuación idéntica del RNG después de restaurar.

En Velocity, las pruebas adicionales ejecutan el JNI de producción, guardan los
20 recursos y el estado completo, recuperan la misma geometría y continúan la
conducción. La integración Android debe validar guardar, reiniciar, recuperar y
volver a conducir, además de medir memoria y tiempo frente a la versión anterior.
