# Volvo Penta MDI → ESP32 → Signal K

Firmware experimental para escuchar el CAN/J1939 de un Volvo Penta D1/D2 con MDI de 2007, enviar telemetría a Signal K e investigar las indicaciones propietarias del cuadro. Incluye diagnóstico web, captura persistente de cambios, marcas humanas sincronizadas y actualización por WiFi.

**No es un diagnóstico Volvo oficial ni un sustituto del cuadro, las alarmas o los procedimientos del fabricante.** El significado de varios bits sigue sin confirmar. CAN se configura en modo **solo escucha**; el proyecto no acciona arranque, parada ni calentadores. No debe utilizarse como única protección del motor.

## Estado de la investigación

Tenemos identificada la telemetría estándar de RPM, horas, temperatura y alimentación del MDI. Hay una asociación fuerte entre `FF89 / C1 / byte 3 / máscara 0x20` y el aviso de baja tensión, pero no una verificación oficial ni una prueba de exclusividad. **La indicación propietaria de presión de aceite aún no está identificada.** Tampoco están demostradas las de temperatura, precalentamiento o parada.

Los índices de byte de este repositorio son **base cero**, salvo indicación expresa. `C1` es un selector en el primer byte, no un PGN. No confundir los bits de salida NMEA 2000 de un gateway con los bits originales del MDI.

- [Hallazgos, hipótesis, incógnitas y fuentes](docs/RESEARCH.md).
- [Cómo utilizar la página de investigación](docs/INVESTIGACION.md).
- [Limitaciones técnicas y tareas pendientes](docs/LIMITACIONES.md).

## Qué hace el código

1. Recibe tramas extendidas CAN mediante TWAI. Aprende 250 o 500 kbit/s y guarda la velocidad; selecciona la fuente de motor a partir de EEC1 válido.
2. Decodifica PGN estándar, convierte a unidades Signal K y publica por WiFi. RPM se publica aproximadamente cada 500 ms; telemetría lenta, cada 2 s.
3. Examina mensajes propietarios sin pretender disponer del mapa completo Volvo. En modo real, publica experimentalmente la asociación C1/baja tensión.
4. Interpreta DM1, incluido transporte multipaquete, si aparece. En nuestras capturas no se ha observado DM1.
5. Guarda automáticamente cambios CAN en SPIFFS, con rotación de dos archivos. Ofrece además un anillo manual en RAM y un registro persistente de observaciones.
6. Presenta diagnóstico e investigación en HTTP `:8080`; mantiene la configuración de SensESP en el puerto 80.
7. Permite subir manualmente un `firmware.bin` por la web o usar OTA desde PlatformIO. No descarga versiones automáticamente.
8. Incluye soporte para sondas Dallas/OneWire y pantalla OLED opcional.

### Telemetría

| PGN decimal | Campo interpretado | Destino / unidad |
| --- | --- | --- |
| 61444 | EEC1, velocidad del motor | `propulsion.main.revolutions`, Hz (RPM/60) |
| 65253 | Horas de funcionamiento | `propulsion.main.runTime`, segundos |
| 65262 | Temperatura refrigerante | `propulsion.main.coolantTemperature`, kelvin |
| 65271 | Potenciales eléctricos válidos | En este MDI, SPN 158 → `propulsion.main.volvoMdi.supplyVoltage`, voltios |
| 65263 | Presión de aceite, si existe | `propulsion.main.oilPressure`, pascales; no observada en este MDI |
| 65276 | Nivel de combustible, si existe | `tanks.fuel.main.currentLevel`, fracción |
| 65226 | DM1 / DTC, si existe | Alarmas derivadas de SPN/FMI y datos de diagnóstico |
| 65417 | Multiplexado propietario FF89 | Datos crudos e interpretación experimental C1 |

Soportar un PGN no significa que este motor lo transmita. La alimentación del MDI no equivale necesariamente a una medición en bornes de batería o alternador.

Las alarmas para el consumidor Android se publican como booleanos en rutas como `propulsion.main.lowVoltageAlarm`, `lowOilPressureAlarm` y `overTemperatureAlarm`. Son convenciones de esta integración, **no** mensajes estándar `notifications.*`. La app Android no forma parte de este repositorio. No se deduce una alarma de aceite simplemente porque RPM sea cero; las reglas orientativas del panel web no son automáticamente alarmas publicadas en Signal K.

## Hardware y puesta en marcha

Configuración incluida: ESP32 clásico (`esp32dev`), CAN TX GPIO32 / RX GPIO34, OneWire GPIO4, I²C SDA16 / SCL17, OLED SSD1306 opcional 128×64 en `0x3c`. Adaptar al hardware real. Es necesario un transceptor CAN adecuado: **no conectar CAN-H/CAN-L directamente al ESP32**. Revisar alimentación, masas y terminación del bus antes de conectar; no añadir terminación sin comprobar la existente.

Se utiliza PlatformIO, plataforma `espressif32@6.5.0`, Arduino y SensESP `3.5.0`. Las dependencias están en `platformio.ini`. El script previo a compilación parchea la persistencia de esa versión concreta de SensESP: escritura temporal, comprobación y sustitución con respaldo; también comprueba recepción completa del cuerpo HTTP y errores al guardar. No se debe actualizar SensESP sin revisar este parche.

Comandos para quien compile e instale su propio firmware:

```sh
pio run -e sh-esp32
pio run -e sh-esp32 -t upload --upload-port PUERTO_USB
pio device monitor --baud 115200
```

`sh-esp32` es el modo real. `sh-esp32-test` genera escenarios sintéticos locales: **sus máscaras no describen el protocolo Volvo** y no sirve para validar un motor. `sh-esp32-ota` permite cargar remotamente con `MDI_OTA_PASSWORD` en el entorno, sin guardar la contraseña en el repositorio.

La tabla de particiones requiere flash de 4 MiB y reserva dos aplicaciones OTA y 512 KiB de SPIFFS. Un cambio inicial de particiones requiere USB y puede obligar a reconfigurar el equipo; una actualización OTA de aplicación no cambia la tabla. Descargar las capturas antes de intervenir.

## WiFi, páginas y actualización

Configurar WiFi y Signal K en `http://IP_DEL_ESP32/`. El AP se llama `sh-esp32-volvo-mdi`; normalmente se accede por `192.168.4.1` cuando se está conectado a él. En la red del barco se utiliza la IP asignada al ESP32. Este texto no implica que deba cambiarse la red del ordenador para trabajar.

No hay contraseña compartida publicada: el firmware genera una clave inicial aleatoria y persistente; la configuración permite sustituirla. La clave efectiva del AP es también la de OTA. Consultar el registro serie inicial para la clave generada; una contraseña personalizada guardada no se vuelve a imprimir como clave inicial.

En `http://IP_DEL_ESP32:8080/` se pueden ver tramas, estados, descargar/borrar capturas automáticas e iniciar capturas manuales. El botón **Investigación** abre una página adicional sin sustituir el panel existente.

Para actualizar por web: seleccionar el binario correspondiente al modo real, introducir la clave AP/OTA, pulsar **Comprobar clave** y después **Subir e instalar**. El progreso avanza en tramos del 10%; el 100% se muestra tras la respuesta satisfactoria del equipo. No cortar la alimentación. No es un sistema de actualización automática desde GitHub.

**Seguridad:** las páginas y descargas son accesibles en la red local; HTTP no cifra datos. Las operaciones de modificación usan sesión/CSRF y la carga de firmware exige además contraseña. No exponer los puertos a Internet ni considerar esto un panel endurecido para redes no fiables.

## Organización y publicación

- `src/main.cpp`: CAN, decodificación, Signal K, WiFi y configuración.
- `src/diagnostic_web.cpp` y `.h`: servidor, interfaz, capturas e investigación.
- `scripts/patch_sensesp_persistence.py`: adaptación de SensESP 3.5.0.
- `partitions_ota.csv`: distribución de flash.

Esta publicación es una instantánea del código de trabajo de septiembre de 2026. Se han eliminado las rutas locales de compilación del archivo PlatformIO; no se incluyen claves, tokens, capturas privadas, manuales de terceros ni binarios. La versión instalada en un dispositivo puede ser distinta. La publicación no supone una nueva prueba de compilación ni validación en motor.

## Colaborar

Buscamos capturas sincronizadas con **observaciones físicas independientes**, especialmente luz de aceite, luz de carga y símbolo de precalentamiento. En una incidencia indicar modelo/año MDI, versión de firmware, PGN/selector/bytes, secuencia relativa a ON, temperatura inicial y qué se observó realmente. Anonimizar capturas antes de compartir y separar hipótesis de hechos. No provocar baja presión, sobretemperatura ni otros fallos peligrosos para obtener datos.

El repositorio es público; todavía no se ha elegido una licencia general para el código propio. Publicarlo no implica conceder una licencia de reutilización irrestricta. Los componentes de terceros conservan sus licencias; véase [THIRD_PARTY.md](THIRD_PARTY.md).
