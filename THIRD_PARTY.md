# Componentes de terceros

El firmware depende de SensESP, Arduino-ESP32 y las bibliotecas enumeradas en `platformio.ini`, que PlatformIO obtiene por separado. Sus licencias no se sustituyen por la de este repositorio.

`scripts/patch_sensesp_persistence.py` contiene fragmentos de SensESP 3.5.0 y modificaciones locales de persistencia y lectura HTTP. SensESP se distribuye bajo Apache License 2.0; se conserva una copia en `licenses/SensESP-LICENSE`. Fuente: https://github.com/SignalK/SensESP. El script documenta las sustituciones respecto al original; no es un archivo oficial de SensESP.

Los manuales Volvo y Yacht Devices se enlazan, no se redistribuyen. Las marcas pertenecen a sus respectivos titulares. Este proyecto no está afiliado ni respaldado por Volvo Penta, Yacht Devices o Signal K.

La licencia general del código propio está pendiente de elección por su titular. Un repositorio público no equivale automáticamente a una licencia de código abierto.
