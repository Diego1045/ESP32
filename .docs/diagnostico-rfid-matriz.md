# Diagnóstico RC522 — matriz y conclusiones

Fecha: pruebas automáticas en banco (`/dev/cu.usbserial-120`).  
Cableado documentado: CS=23, CLK=26, MOSI=25, MISO=27, RST=22, 3.3V, GND.

## 1. Capa aplicación (sin radio)

| Prueba | Resultado |
|--------|-----------|
| `tap 44CBC871` + `format on` | **OK** — cola access, mensajes `[RFID]`, intento de provisionar sector 1 |
| Formateo sin RF real | Falla esperada en *re-selección* MIFARE (no hay PICC en antena) |
| Relé / `unlock` | Perfil consola ya activo |

**Descartado:** bug general en NVS, access_task, CLI o flujo formateo. El firmware de cerradura responde; el cuello de botella es **RFID físico**.

## 2. Capa SPI digital

| Prueba | swap=0 | swap=1 (software) |
|--------|--------|-------------------|
| Registro versión (`fw` / `selftest`) | Inestable (a menudo 0x00) | **0x92 estable** |
| Test FIFO (`selftest fifo_rw`) | Falla | **ESP_ERR_INVALID_ARG** / mismatch |
| `TxControl` antena | — | **0x83, RF ON** |
| Scans 18× (matriz) | F527 (RX timeout) | 100 kHz: F523/F526 (campo); 500 kHz–1 MHz: F528 (sin respuesta) |

**Conclusión SPI:** CS/SCK/RST/3.3V llegan al chip con **swap=1** (MISO/MISO intercambiados en firmware). Las transferencias **multibyte (FIFO)** no son fiables → SELECT/CRC/MIFARE fallan aunque a veces haya colisiones REQA.

**Descartado:** “RC522 muerto” o CS en pin equivocado del ESP (con swap=1).  
**No descartado:** MOSI/MISO mal respecto al módulo (probar **cruce físico** + `HW_SPI_SWAP_MOSI_MISO=0`), contactos flojos, o módulo clone defectuoso.

## 3. Matriz velocidad SPI (18 scans c/u)

| Config | Errores scan dominantes | Sondeo auto (durante test) |
|--------|-------------------------|----------------------------|
| swap=0 @ 100 kHz | F527 | — |
| swap=0 @ 500 kHz | F527 | — |
| swap=1 @ 100 kHz | F523, F526 | colisión en sondeo |
| swap=1 @ 500 kHz | F528 | timeouts |
| swap=1 @ 1 MHz | F528 | timeouts |

Ninguna combinación obtuvo **UID_OK** en la matriz. La señal RF es **intermitente** (en otras sesiones hubo cientos de REQA ok y SELECT con CRC).

## 4. Comandos nuevos en monitor

- `selftest` — versión, FIFO, antena, pines  
- `tap <UIDhex>` — emula tarjeta sin RF (banco / depuración lógica)

## 5. Qué hacer en hardware (orden)

1. **Cruzar cables MOSI y MISO** entre ESP32 y módulo; en `hardware_profile.h` poner `HW_SPI_SWAP_MOSI_MISO 0`; flashear; `selftest` debe mostrar `fifo_rw=OK` y `version=0x92`.
2. Revisar **3.3 V** (no 5 V) y **GND** común.
3. Tarjeta **quieta en el centro** de la antena; repetir `scan` 10 veces seguidas.
4. Si `fifo_rw=OK` pero `scan` sigue en F528 → sospecha **antena/módulo** (no el código de auth).

## 6. Config recomendada en firmware (hasta cruzar cables)

- `HW_SPI_SWAP_MOSI_MISO 1`  
- SPI **500 kHz**  
- Ganancia RX **48 dB** al arrancar  

Script de matriz: `python3 tools/rfid_matrix_test.py`
