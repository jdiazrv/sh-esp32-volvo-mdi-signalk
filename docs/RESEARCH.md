# Investigación MDI 2007: qué sabemos y qué falta

Estado documental: 14 de septiembre de 2026. Son resultados de una instalación concreta D1/D2 con MDI de 2007; no una tabla oficial Volvo ni una garantía para otros motores, coches o camiones.

## Método y alcance

El análisis acumulado local maneja 20 archivos, algunos duplicados, con 6.368 filas únicas y 15 sesiones con CAN. **Sesión de arranque del ESP32 no significa arranque de motor**. Muchas filas carecen de UTC válido: la fecha de descarga no fecha necesariamente el suceso. Se correlaciona dentro de cada sesión mediante `boot_id` y `uptime_ms`, no mezclando relojes de reinicios distintos.

Los CSV privados no se distribuyen en esta publicación. Por tanto las cifras siguientes son un resumen del análisis local, no un conjunto de resultados completamente reproducible sólo con este repositorio. Se necesitan muestras anonimizadas y ensayos independientes para afianzar el mapa.

## Grados de evidencia

| Campo | Estado | Qué permite afirmar |
| --- | --- | --- |
| EEC1 61444, RPM | Estándar e identificado en capturas | Bytes 3–4 LE ×0,125 RPM |
| 65253, horas | Estándar e identificado | Primeros cuatro bytes LE ×0,05 h |
| 65262, refrigerante | Estándar e identificado | Byte 0 menos 40 °C |
| 65271, SPN158 | Identificado | Bytes 6–7 LE ×0,05 V: alimentación del MDI, no necesariamente tensión directa de alternador |
| FF89 byte 0 | Observado | Selector de registros; no interpretar todas las tramas como una única máscara |
| FF89 byte 1 | Observado | Contador rodante en las muestras; se excluye del disparador de captura |
| FF89 selector 07 | Concordancia empírica | Bytes 4–5 LE ×0,1 h concuerdan con las horas estándar |
| FF89 A0, bytes 2–3 `FA 9F` | Asociación acústica | Se relaciona con señalización sonora; no identifica por sí solo cada pitido ni fin de precalentamiento |
| FF89 C1, byte 3 `& 0x20` | Hipótesis fuerte | Asociado a baja tensión; utilizado experimentalmente por el firmware |
| FF89 B2, bit `0x08` | Ambiguo | Aparece en contextos de arranque y alarma; no es una prueba exclusiva de START |
| FF89 B4 | Ambiguo | Cambios próximos a arranque/carga, sin semántica completa |
| Aceite, temperatura, precalentamiento, STOP propietarios | No identificados | No hay máscara fiable que permita reproducir todas las luces del cuadro |

Todos los índices de esta tabla son base cero. No trasladar a FF89 los números de bits 2, 3 o 6 de las alarmas **de salida NMEA 2000** del gateway Yacht Devices.

## Baja tensión: evidencia y contraejemplos

En una sesión, siete episodios mostraron activación C1/0x20 entre 9,18 y 10,38 s después de caer la alimentación a ≤13 V. Otra tuvo cuatro episodios: dos próximos a 10 s y otros dos de unos 33 y 49 s. No deben ocultarse esos desacuerdos. En la revisión del 13 de septiembre se observaron estas dos transiciones, en segundos desde el arranque del ESP32:

| Cruce observado ≤13 V | Activación C1/0x20 | Diferencia |
| ---: | ---: | ---: |
| 2785,969 | 2795,860 | 9,891 s |
| 3101,909 | 3110,499 | 8,590 s |

La coincidencia con la regla de aproximadamente 10 s del manual es fuerte, pero no demuestra que sea un bit exclusivo de carga y no una indicación compartida. Filtros de captura y periodicidad de las mediciones limitan la precisión. La desaparición cerca de la recuperación de tensión no establece una histéresis exacta.

**Corrección histórica:** anteriormente se propuso STOP para esta transición. Esa interpretación ya no se sostiene como exclusiva. Una confirmación antigua del usuario no valida el significado del bit si la pregunta estaba mal etiquetada. En esta instalación el solenoide de parada está roto y el motor se detiene mecánicamente; pulsar STOP eléctrico y detener el motor son eventos diferentes.

## Aceite: por qué no lo damos por resuelto

No se ha observado DM1 ni PGN 65263 de presión en las capturas analizadas. El sensor descrito es un interruptor, no una medida numérica de presión. El patrón C1 `BC 00` aparece tanto con motor parado como funcionando a 883,5 y 1.896 RPM: ese patrón no basta para identificar baja presión. Otros registros, como 05, no aportan aún una transición discriminante.

El manual de taller describe contacto cerrado por debajo de 60 kPa y temporizaciones de alarma de más de 30 s por debajo de 1.000 RPM y más de 0,5 s por encima de 1.000 RPM. **Eso no establece por sí solo qué debe iluminarse inmediatamente al dar ON.** Quedan por comprobar prueba de lámparas, inhibición antes del primer arranque, caso exacto de 1.000 RPM y condiciones de borrado/histéresis. No se debe afirmar ni que siempre deba saltar al dar ON ni que nunca deba hacerlo.

## Precalentamiento y pitidos

El manual describe una condición de refrigerante por debajo de 50 °C y una secuencia con START/PREHEAT; también existe redacción sobre activación al conectar el sistema que requiere distinguir versión y mando concreto. No basta con oír dos pitidos para concluir que terminó el calentamiento.

En el experimento humano se dio ON, se esperaron dos pitidos y se pulsó/sueltó arranque; en otro ensayo se esperaron aproximadamente 15 s después del pitido. Estas son observaciones de secuencia, no una medida directa del relé de calentadores. Los valores A0 `2C 01` y `D0 07` podrían representar 300 y 2.000 ms, pero otros valores impiden afirmar que el campo sea siempre una duración.

## Qué queremos confirmar

1. **Aceite:** anotar por separado luz física encendida/apagada, ON, giro de arranque y motor funcionando; comparar frío/caliente y parada mecánica. No inferir el estado de la luz a partir de RPM.
2. **Carga:** observar la luz real y tensión del MDI durante avisos naturales; comprobar si C1/0x20 cambia sin otros avisos y si puede aparecer por otro motivo. El manual describe ≤13 V durante más de 10 s con motor funcionando y ≥15 V durante más de 30 s; los paneles solares no demuestran por sí solos la causa.
3. **Precalentamiento:** registrar temperatura inicial, botón utilizado y símbolo real, además de inicio/fin de sonido. Diferenciar energización del calentador de aviso acústico.
4. **STOP:** comparar ON→STOP sin arrancar, pulsación de STOP con motor funcionando y parada mecánica, documentando el solenoide averiado. No llamar STOP a cualquier estado de motor parado.
5. **Silenciar alarma:** comparar la misma secuencia sin pulsar y pulsando dos veces separadas 5 s. Un cambio C1 `BC20→9C20` cerca del fin de sonido puede ser reconocimiento/silencio, pero aún no está demostrado.

No provocar averías para confirmar hipótesis. Un profesional puede plantear pruebas de sensores siguiendo el procedimiento del fabricante si fueran necesarias.

## Lo que no sabemos

Mapa propietario completo, exclusividad de C1/0x20, bits de aceite y temperatura, estado real de calentadores, semántica completa de B2/B4/90/91/05/20, otros PGN propietarios y diferencias entre generaciones. Tampoco se ha encontrado en lo revisado un decodificador público que cierre estas incógnitas. Compartir J1939 con camiones Volvo no demuestra que compartan estos campos propietarios.

## Fuentes y comparación

- [Manual de taller Volvo Penta MDI 7748428, copia alojada en Plaisance Pratique](https://www.plaisance-pratique.com/IMG/pdf/MID.pdf): referencia de sensores y lógica de avisos; consultar numeración impresa, especialmente aceite p. 21, precalentamiento pp. 37–39 y tensión pp. 40–41. La aplicabilidad exacta al mando instalado sigue siendo parte del contraste.
- [Manual Yacht Devices YDEG-04](https://www.yachtd.com/downloads/ydeg04.pdf): demuestra la integración de datos de motor y avisos hacia NMEA 2000; no proporciona una tabla completa del CAN propietario original que podamos trasladar sin validación.
- [ieb/EngineManagement](https://github.com/ieb/EngineManagement): trabajo de gestión/reemplazo; no lo presentamos como validación de nuestras máscaras.
- [ieb/EngineMonitor](https://github.com/ieb/EngineMonitor): monitorización de motor; tampoco prueba el mapa propietario del MDI.
- [Signal K / SensESP](https://github.com/SignalK/SensESP): infraestructura utilizada por el firmware, no fuente de la semántica propietaria Volvo.

Las reglas de manual, las observaciones humanas y los bytes CAN son tres fuentes distintas. Una hipótesis debe concordar con las tres sin convertir la regla del manual en una supuesta observación del bus.
