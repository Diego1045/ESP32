# Qué es el sistema Cerradora Inteligente

La **Cerradora Inteligente** (nombre de proyecto: `esp32-secure-lock`) es un control de acceso físico sobre ESP32. En lugar de abrir con un UID RFID copiable, exige que la tarjeta sea MIFARE Classic, se autentique con Crypto-1 en un sector protegido y, en el diseño previsto, presente un HMAC ligado a un nonce que cambia tras cada uso válido.

Hoy el firmware corre en un ESP32 DevKit, lee tarjetas con un RC522 por SPI y, si el acceso se concede, activa un relé unos 2 segundos para disparar la cerradura o el mecanismo asociado.

## Qué problema resuelve

Una cerradura RFID típica compara el UID de la tarjeta con una lista. Ese UID se puede clonar. Este sistema añade dos capas:

1. **Autenticación de sector MIFARE** — solo una tarjeta formateada con la clave secreta del Sector 1 puede hablar con el lector.
2. **HMAC + nonce (anti-replay, diseño previsto)** — el acceso debería depender de `HMAC-SHA256(UID + nonce)` con una master key en NVS. Tras un acceso válido, el nonce se rota para que una captura previa no sirva otra vez.

## Hardware

| Pieza | Función |
| --- | --- |
| ESP32 DevKit | Ejecuta el firmware, guarda secretos en NVS y decide si abrir. |
| RC522 (MFRC522) | Lector RFID ISO 14443A / MIFARE por SPI. |
| Módulo relé | Salida de 2 s en GPIO 2 para accionar la cerradura. |
| Tarjetas MIFARE Classic | Credenciales. Otras tarjetas se rechazan. |

Cableado (coincide con `diagram.json` y `main/main.c`):

| Señal RC522 | GPIO ESP32 |
| --- | --- |
| SCK | 18 |
| MISO | 19 |
| MOSI | 23 |
| SDA / CS | 5 |
| RST | 22 |
| RST | no conectado (soft-reset) |
| Relé IN | 2 |

El SPI del RC522 usa 1 MHz para clones y cableado físico.

## Software

Proyecto ESP-IDF (también compilable con PlatformIO, entorno `esp32dev`).

| Módulo | Rol |
| --- | --- |
| `main/main.c` | Arranque, GPIO del relé, eventos del RC522, modos formateo/producción. |
| `main/security.c` | HMAC-SHA256 (UID + nonce) con master key; rotación de nonce. |
| `main/storage.c` | Persistencia NVS (`lock_data` / `settings`): master key de 32 bytes y nonce de 16 bytes. |
| `abobija/rc522` | Lector: sondeo, ciclo de vida de la tarjeta, Auth/Read/Write MIFARE. |

Al arrancar:

1. Inicializa NVS y carga `lock_settings_t`.
2. Deja el relé en bajo y crea un timer de 2 s para apagarlo.
3. Instala el driver SPI del RC522 y espera eventos `PICC_STATE_CHANGED`.

Si no hay settings en NVS, usa valores por defecto (master key `0xAA…` y nonce `0x00…`). Esos valores son de desarrollo, no de producción.

## Dos modos de operación

El flag `format_mode` en `main.c` elige el comportamiento al presentar una tarjeta Classic.

### Modo formateo (`format_mode = true`)

Sirve para **dar de alta** tarjetas nuevas:

1. Autentica el tráiler del Sector 1 (bloque 7) con la clave de fábrica `FF FF FF FF FF FF`.
2. Escribe un tráiler nuevo: Key A secreta, bits de acceso estándar y Key B por defecto.
3. No abre el relé.

Después de formatear las tarjetas hay que poner `format_mode` en `false` y volver a flashear.

### Modo seguro / producción (`format_mode = false`)

Flujo de acceso:

1. La tarjeta pasa a estado ACTIVE.
2. Se ignora un segundo evento del mismo UID mientras la tarjeta sigue sobre el lector.
3. Si no es MIFARE Classic compatible, se rechaza y se envía HALT.
4. Autenticación Crypto-1 del bloque 4 (primer bloque de datos del Sector 1) con Key A secreta.
5. Si Auth falla → acceso denegado.
6. Si Auth ok → se calcula un HMAC y se llama a `security_validate_and_update_card`.
7. Si la validación es 0 → relé ON 2 s, se recarga settings (nonce nuevo) y se hace HALT.

## Modelo de seguridad (diseño)

```
Tarjeta MIFARE          ESP32
     |                     |
     |  UID + Crypto-1     |
     |  (Sector 1, Key A)  |
     |-------------------->|
     |                     | Auth MIFARE OK
     |  hash en tarjeta    | HMAC(UID, nonce, master_key)
     |  (previsto)         | comparar
     |                     | si ok: nonce nuevo en NVS
     |                     | relé 2 s
```

- **Key A del Sector 1**: no basta clonar el UID; hace falta la clave MIFARE.
- **Master key (32 B) + nonce (16 B)** en NVS: el HMAC no debe ser reproducible sin esos secretos.
- **Rotación de nonce**: un dump o un replay de una sesión anterior debería quedar inválido.

## Estado actual respecto al diseño

La autenticación MIFARE (capa 1) está activa.

La capa HMAC **aún no lee un hash de la tarjeta**. En `main.c` el “hash recibido” se calcula en el ESP32 con el mismo UID, nonce y master key que usa la validación. Esa comparación siempre coincide si Auth MIFARE fue bien. El nonce sí se rota en NVS tras un acceso concedido, pero **no se escribe de vuelta en la tarjeta**.

En resumen: hoy el sistema es una **cerradura RFID con sector MIFARE protegido**; el HMAC anti-replay está preparado en `security.c` y todavía no está cerrado el ciclo tarjeta ↔ dispositivo.

## Límites y riesgos conocidos

- El UID **no** se usa como prueba de identidad por sí solo (correcto: hay tarjetas “magic” que lo cambian).
- Secretos (Key A, master key por defecto) están en el firmware; hay que cambiarlos fuera de desarrollo.
- `format_mode` en el binario es un interruptor de mantenimiento: no debe quedar en `true` en campo.
- Tarjetas no Classic (NTAG, Ultralight, etc.) no entran.
- El relé es un pulso fijo de 2 s; no hay sensor de puerta ni estado abierto/cerrado.

## Cómo se usa en la práctica

1. Flashear con `format_mode = true` y pasar cada tarjeta Classic nueva.
2. Confirmar en log `TARJETA FORMATEADA`.
3. Flashear con `format_mode = false`.
4. Acercar una tarjeta formateada: Auth OK → relé; tarjeta ajena o sin clave → denegado.

Logs habituales: `MAIN`, `SECURITY`, `STORAGE` y `rc522` (nivel INFO).

## Documentación técnica adicional

- **[Optimizaciones RC522 y diagnóstico de logs](./optimizaciones-rc522.md)** — códigos `F528`/`F52C`, mejoras propuestas (sin implementar todas) y checklist one-shot.
- **[Banco solo RFID (consola)](./banco-solo-rfid.md)** — sin relé: comandos `format on`, `logs`, `unlock` por monitor serie.

### Aviso rápido: `REQA failed … (err=F52C)`

`F52C` = error de **paridad RF** en REQA (ruido, tarjeta mal apoyada, cableado o EMI del relé). Suele ser transitorio; si el acceso sigue funcionando, es sobre todo ruido en el log. Detalle y plan de mitigación en el documento de optimizaciones.
