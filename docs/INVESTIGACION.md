# Página de investigación

Abrir `http://IP_DEL_ESP32:8080/` y pulsar **Investigación**, o ir a `/research`. Es una página adicional: el diagnóstico, la captura manual y la actualización permanecen disponibles en el panel principal.

## Para qué sirve

Une tres cosas que no deben confundirse: **trama capturada**, **hipótesis del firmware** y **observación humana**. Ver «posible arranque» en pantalla no significa que el protocolo esté descifrado. Responder «sí» mucho después tampoco fija la hora física del arranque: para eso están los botones de observación.

## Durante una maniobra

1. Mantener abierta la página y esperar «Reloj sincronizado».
2. Usar los botones al observar ON, inicio/fin de precalentamiento, inicio/fin de pitidos, pulsación de START, fin del giro de arranque, motor funcionando, pulsación de STOP y motor detenido.
3. Marcar sólo lo que se puede reconocer directamente. Si no se ve el símbolo de calentamiento, no marcar su fin por haber terminado el pitido.
4. Realizar la maniobra con seguridad. No intentar contestar varias hipótesis mientras se controla el motor.
5. Después, desplegar las hipótesis recientes y confirmar o rechazar sólo las inequívocas. Se muestran hasta 16 recientes; no constituyen una lista de hechos confirmados.
6. Descargar `/research.csv`, `/auto-capture.csv` y `/auto-capture-previous.csv` antes de borrar o repetir muchos ensayos.

En esta instalación STOP mecánico es distinto del botón STOP: no marcar «pulso STOP» si sólo se ha tirado del mando mecánico. Marcar «motor detenido» y acompañar el ensayo de una nota sobre cómo se paró. Actualmente faltan botones específicos de parada mecánica, reconocimiento de alarma y luz de aceite/carga en esta página. El panel principal sí tiene marcas de alarmas ligadas a la captura manual; esta limitación debe tenerse en cuenta al comparar archivos.

## Latencia y relojes

La página captura `pointerdown` con `performance.now()` y lo convierte al tiempo de actividad del ESP32. Al abrir hace cinco peticiones de sincronización y conserva la de menor ida y vuelta; después consulta cada 500 ms. La etiqueta ±RTT/2 es una **estimación de red**, no una garantía de precisión ni una corrección del tiempo de reacción de la persona.

La marca conserva el instante del toque aunque la petición llegue después. El servidor valida el tiempo y puede responder antes de que la cola termine de escribirse en flash: «GUARDADA» no garantiza que una pérdida inmediata de alimentación no pierda la última marca. Las respuestas sí/no tienen su propio tiempo y no sustituyen el del evento original.

`uptime_ms` es tiempo desde arranque del **ESP32**, no desde ON del MDI ni desde arranque del motor. Para comparar ensayos hay que restar la marca ON de cada secuencia. Unir por `boot_id`; UTC sólo es utilizable cuando está sincronizado. Reiniciar el ESP32 cambia el origen temporal.

## Hipótesis automáticas actuales

- Primera actividad del MDI: un identificador histórico contiene `preheat_start`, pero el texto advierte que **no confirma precalentamiento**.
- A0 `FA 9F`: asociación con señalización acústica, no cada pitido individual.
- B2/0x08: candidato a START bajo ciertas condiciones; no es exclusivo. Se limita cuando C1/baja tensión está activo.
- C1/0x20: candidato fuerte a aviso de baja tensión. El significado histórico «STOP» se corrigió; no reinterpretar confirmaciones antiguas sin contexto.

## Capturas y espacio

La captura automática funciona sin pulsar «capturar». Conserva línea base y cambios relevantes, excluyendo el contador rodante y cambios exclusivos de RPM. La versión publicada conserva para temperatura la línea base y cruces del umbral de 50 °C; no es un registro continuo de cada grado. Se usa una clave por PGN/origen y, para ciertos PGN propietarios, selector.

Rota dos archivos de aproximadamente 128 KiB y reserva 64 KiB libres. Si no puede escribir, puede pausar; **no es una captura CAN sin pérdidas**. Consultar [limitaciones](LIMITACIONES.md) antes de medir secuencias rápidas.

Columnas del CSV automático: `boot_id`, `utc`, `epoch_ms`, `uptime_ms`, `event`, `can_id_hex`, `pgn`, `pgn_hex`, `source_hex`, `length`, `raw_changed_mask`, `trigger_mask`, `xor_hex`, `data_hex`. El XOR compara con la trama anterior recibida de esa clave, no necesariamente con la anterior fila guardada.

La captura manual es un anillo de 160 filas en RAM: pierde datos al reiniciar y puede sobrescribir filas antiguas. No sustituye a la automática. El registro de investigación persiste por separado; algunos archivos históricos pueden carecer de cabecera.

«Borrar capturas automáticas» elimina únicamente los dos CSV automáticos, no WiFi ni Signal K ni el registro de investigación. Descargar y verificar los archivos antes de borrar: el borrado en el ESP32 no es recuperable desde esta interfaz.
