#ifndef EVENT_TX_H
#define EVENT_TX_H

// Búfer de eventos compartido y tarea que los transmite por la UART en
// segundo plano. El calendarizador solo encola (sección crítica breve); nunca
// escribe en la UART.

#include <stddef.h>
#include <stdint.h>

// Crea la tarea de transmisión (núcleo 0, prioridad baja). false si falla.
bool event_tx_init(void);

// Vacía el búfer y los contadores. Llamar solo con la transmisión en reposo
// (antes de Start, tras un Stop que ya hizo flush).
void event_tx_reset(void);

// Encola un evento. Seguro desde cualquier tarea (no desde ISR).
void event_tx_push(uint8_t type, uint32_t tick, uint16_t id, uint16_t arg);

// Deja listo un TickReport (carga ya codificada, máx. 32 B). Si había otro
// sin enviar, se reemplaza (solo importa el más reciente). Se transmite
// después de los eventos que ya estén en el búfer.
void event_tx_post_report(const uint8_t *payload, size_t length);

// Avisa a la tarea de transmisión que hay eventos o un reporte pendientes.
void event_tx_kick(void);

// Bloquea hasta que todo lo encolado se haya entregado al driver UART.
void event_tx_flush(void);

uint32_t event_tx_dropped(void);

#endif