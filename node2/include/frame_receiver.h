// Junta los bytes que llegan por USB hasta tener una trama completa.
// El USB puede entregar media trama, una trama o varias pegadas, asi que
// se le pasan los bytes de a uno con push().
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

class FrameReceiver {
public:
    FrameReceiver();

    // Agrega un byte. Devuelve true cuando se completo una trama valida
    // (y la deja en frame).
    bool push(uint8_t byte, Frame *frame);

    // Descarta una trama a medias (por ejemplo si dejaron de llegar bytes)
    void reset();

    // Hay una trama a medio recibir
    bool in_progress() const;

    // Contadores: tramas válidas, descartadas por CRC malo
    // y descartadas por largo mayor a 512
    uint32_t valid_frames() const;
    uint32_t crc_errors() const;
    uint32_t length_errors() const;

private:
    uint8_t buffer_[FRAME_MAX_SIZE];
    size_t count_; // bytes guardados de la trama actual
    size_t expected_; // tamaño total esperado (se sabe al leer el largo)
    uint32_t valid_frames_;
    uint32_t crc_errors_;
    uint32_t length_errors_;
};
