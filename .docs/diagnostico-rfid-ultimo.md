# Validación tras nuevo cableado MOSI/MISO

Fecha: pruebas automáticas. Tarjeta sobre el lector. Puerto `/dev/cu.usbserial-120`.

## Resultado

El cableado actual **coincide con `HW_SPI_SWAP_MOSI_MISO=1`** (firmware ya flasheado, SPI **100 kHz**).

| swap | Hz | versión | antena | 25× scan | sondeo |
|------|-----|---------|--------|----------|--------|
| 0 | 100k | **0x00** | OFF | F527 | sin respuesta |
| 0 | 500k | **0x00** | OFF | F527 | sin respuesta |
| **1** | **100k** | **0x92** | **ON** | F529×16, F524×9 | **203 REQA ok**, 0 UID |
| 1 | 500k | 0x92 | ON | F528 / F52C / F526 | inestable |

**swap=0 no sirve** con este cableado (el chip no responde). No dejes esa opción.

## Qué está bien

- SPI de control: versión **0x92**, antena RF encendida.
- La tarjeta **está en el campo** (REQA ok ~200, errores de SELECT no de “sin tarjeta”).

## Qué sigue mal

- Test FIFO **falla**.
- SELECT no entrega UID: **F524** (CRC) y **F529** (SAK inválido).
- Cero lecturas `UID=` en toda la matriz.

## Comandos

`selftest` · `scan` · `gain 48` · `tap 44CBC871` (lógica sin RF)
