# Virtual qSSD

- Conservar el objetivo: espacio lógico por reglas conectado a CPU XI/Ether y
  GPU-VIRTUAL, con una autoridad y cambios de estado coherentes.
- Usar los núcleos propios de X fijados en dependencies.json. No sustituirlos
  por un simulador externo ni modificar silenciosamente las dependencias.
- Conservar regiones, detalles, fases, correlaciones, revisiones y RNG.
- Operar sobre la representación compacta cuando lo admita el perfil.
- Distinguir enlaces lógicos, teletransporte de Bell y acceso a GPU física.
  No presentar una referencia compartida como entrelazamiento físico.
- Contabilizar por separado proyecciones explícitas, metadatos, checkpoint y
  memoria residente. No afirmar capacidad o velocidad ilimitadas.
- Ejecutar CTest después de cambios de implementación y los controles originales
  relevantes cuando se cambie la integración de los núcleos.
- Mantener la vista de una revisión válida durante trabajos en curso; una nueva
  adquisición debe observar la versión actual de la autoridad común.
- Conservar el crecimiento dinámico de QSSD por defecto: sin cuotas fijas de
  historial, marcadores, objetos, puertos, descripciones o checkpoints. Una
  retención finita debe ser una elección explícita del consumidor. Validar
  integridad, dimensiones reales y desbordamientos sin reintroducir esos topes.
