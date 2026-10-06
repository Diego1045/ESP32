# Banco de pruebas: solo tarjeta + RC522

Sin relé ni botón BOOT cableados. El **monitor serie** (115200) sustituye el actuador y el modo formateo.

## Conexión mínima

| RC522 | ESP32 |
| --- | --- |
| SCK | 26 (D26) |
| MISO | 27 (D27) |
| MOSI | 25 (D25) |
| SDA/CS | 23 (D23) |
| RST | 22 (D22) |
| 3.3V / GND | 3V3 / GND |

## Monitor

```bash
pio device monitor -b 115200
```

En el prompt `lock>` escribe comandos (Enter). Si no ves el prompt, pulsa Enter una vez.

## Flujo con una tarjeta

1. `format on` — provisiona sector 1 + HMAC en bloques 4–5.
2. Acerca la tarjeta hasta ver `>> Tarjeta provisionada` o `FORMAT_OK` en logs.
3. `format off` (o espera 120 s).
4. Acerca la tarjeta de nuevo → debe aparecer  
   `*** [SIM] RELÉ ON 2000 ms — ACCESO CONCEDIDO ***`
5. `logs` — historial en NVS.

> Tras `format off` con la tarjeta aún sobre el lector **no se vuelve a procesar**
> (la tarjeta sigue en estado ACTIVE). Retírala hasta ver `Tarjeta retirada` y
> acércala otra vez.

## Qué debes ver en el monitor al acercar la tarjeta

```
I (...) MAIN: Tarjeta detectada: UID=xx xx xx xx
I (...) ACCESS: Procesando UID=... (formateo=ON|OFF)
```

Diagnóstico según lo que falte:

| Síntoma | Causa probable |
| --- | --- |
| No sale `Tarjeta detectada` | Ejecuta `diag` con la tarjeta puesta (ver abajo). |
| `Misma tarjeta ya procesada` | Retírala hasta `Tarjeta retirada`, o `format on/off` para reprocesarla. |
| Sale `Tarjeta detectada` pero nada más | Cola de acceso/mutex; ver `ACCESS`/`rc522` en WARN. |
| `DENEGADO: auth MIFARE` | Tarjeta sin formatear: `format on` y pasarla. |
| `DENEGADO: tarjeta sin HMAC` | Trailer ok pero HMAC vacío: `format on` otra vez. |

La consola lee el UART directamente y hace eco de lo que escribes; confirma la
línea con Enter.

## Diagnóstico con `diag`

1. `diag reset`, con la tarjeta lejos; espera 3 s y `diag` → casi todo debe ser `timeout`.
2. `diag reset`, pon la tarjeta sobre la antena 3 s y `diag`:
   - sube `ok` o `colision` y luego `SELECT ok` → el lector funciona.
   - `colision`/`ok` suben pero `SELECT fallo` también → tarjeta en el borde o ruido.
   - solo sube `timeout` → el lector no ve la tarjeta (3,3 V, SPI, antena).
   - sube `paridad` → RF marginal: acerca/centra la tarjeta.

Nota: una colisión en REQA se trata como "tarjeta presente" (igual que la
librería MFRC522 de referencia); algunos clones RC522 la marcan aun con una
sola tarjeta.

## Comandos

| Comando | Acción |
| --- | --- |
| `help` | Ayuda |
| `status` | Formateo ON/OFF y perfil hardware |
| `diag` / `diag reset` | Contadores del lector (REQA, colisión, timeout, SELECT) |
| `gain 18\|23\|33\|38\|43\|48` | Ganancia RX en vivo (33 por defecto, no persiste) |
| `format on` / `format off` | Provisionar vs producción |
| `logs [N]` | Últimas entradas (default 10) |
| `revoke DEADBEEF` | Revocar UID (hex, sin espacios) |
| `unlock` | Simula apertura sin pasar tarjeta |

## Cuando conectes el relé

En `main/hardware_profile.h`:

```c
#define HW_HAS_RELAY 1
#define HW_ACTUATOR_CONSOLE 0   // opcional: seguir viendo mensajes SIM
```

GPIO **2** → IN del módulo relé.

## Perfil en código

`hardware_profile.h` — `HW_HAS_RELAY 0`, `HW_HAS_BOOT_BUTTON 0`, `HW_ACTUATOR_CONSOLE 1`.
