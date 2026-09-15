# Limitaciones conocidas y trabajo pendiente

Esta publicación documenta el código existente; no convierte sus supuestos en hechos probados. No se ha compilado ni cargado un nuevo firmware como parte de la publicación.

## Interpretación de alarmas

- Nombres internos como `kMdiC1LowVoltageVerified` y `mappingVerified` son heredados y demasiado categóricos. En el modo real activan la interpretación experimental C1/0x20. **No significan que esté verificado todo el mapa Volvo.** Véase [evidencia](RESEARCH.md).
- La frescura propietaria se renueva con cualquier registro 65417, no sólo con C1. Por tanto un C1 antiguo puede parecer reciente si siguen llegando otros selectores. Antes del primer C1 también existe riesgo de publicar el valor inicial falso. Pendiente: validez y tiempo por campo/selector.
- La frescura común del panel puede hacer que «aceite apagado» parezca observado cuando el aceite no está identificado. Pendiente: distinguir desconocido, caducado, inactivo y activo por separado, también en el consumidor Android.
- El soporte DM1 es una capacidad del código, no una prueba de que este MDI lo emita. «Fallo de calentadores» DM1 no equivale a «precalentamiento activo».
- Las máscaras sintéticas de `sh-esp32-test` no sirven como mapa Volvo. Las máscaras configurables se procesan en la rama de pruebas; definir una máscara de aceite no basta para añadir ese decodificador al modo real.
- Las reglas manuales orientativas del panel web no prueban un bit CAN. La disponibilidad de un campo no debe simularse con un cero.

## Registro y sincronización

- Las versiones anteriores filtraban transiciones dentro de un segundo y podían perder una inversión rápida. La corrección elimina ese filtro para cambios relevantes y conserva A→B→A. El contador de cola y los errores de escritura siguen siendo visibles; si una escritura parcial o ambigua ocurre, la captura se pausa hasta vaciar/rotar el archivo para no encadenar CSV corrupto.
- Se excluyen ciertos cambios, se rotan archivos y hay límites de RAM/flash; no es una traza completa del bus. No medir temporizaciones exactas sin considerar este muestreo.
- La interfaz de captura contiene texto histórico que dice que temperatura se excluye: el código publicado incluye la excepción de línea base/cruces de 50 °C. Pendiente: uniformar el texto visible.
- Respuesta HTTP a una marca y persistencia física no son lo mismo. Pendiente: indicar «en cola» y confirmar escritura, sin penalizar el registro del instante del toque.
- La página incorpora observaciones directas de luz de aceite/carga y parada mecánica. Aún falta ejecutar una secuencia limpia y confirmar que la muestra contiene el instante de la transición; una observación sigue sin probar una máscara CAN.

## Operación y seguridad

- CAN es sólo escucha, pero un montaje eléctrico incorrecto aún puede afectar al bus. Usar hardware y procedimientos adecuados.
- HTTP sin cifrar y lecturas accesibles desde la red local: no exponer a Internet. CSRF no sustituye a autenticación integral.
- OTA no cambia particiones, no garantiza recuperación ante corte de alimentación y no es una actualización automática desde un servidor de versiones.
- Las reglas y observaciones del MDI de 2007 no se extrapolan automáticamente a otros módulos Volvo.

Prioridad propuesta: corregir validez/frescura por alarma y pérdida de transiciones; después recoger observaciones físicas de aceite/carga/precalentamiento. No añadir nuevas «alarmas confirmadas» sólo para llenar indicadores de la app.
