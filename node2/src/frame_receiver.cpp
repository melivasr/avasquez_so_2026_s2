#include "frame_receiver.h"
// Recepcion de tramas: se van agregando bytes hasta completar una trama valida


FrameReceiver::FrameReceiver(){
    valid_frames_ = 0;
    crc_errors_ = 0;
    length_errors_ = 0;
    reset();
}

void FrameReceiver::reset(){
    count_ = 0;
    expected_ = 0;
}

bool FrameReceiver::in_progress() const{
    return count_ > 0;
}

uint32_t FrameReceiver::valid_frames() const{
    return valid_frames_;
}

uint32_t FrameReceiver::crc_errors() const{
    return crc_errors_;
}

uint32_t FrameReceiver::length_errors() const{
    return length_errors_;
}

bool FrameReceiver::push(uint8_t byte, Frame *frame){
    // Esperando el primer byte de la marca: lo demás es basura
    if (count_ == 0) {
        if (byte == FRAME_START_1) {
            buffer_[0] = byte;
            count_ = 1;
        }
        return false;
    }

    // Esperando el segundo byte de la marca
    if (count_ == 1) {
        if (byte == FRAME_START_2) {
            buffer_[1] = byte;
            count_ = 2;
        } else if (byte != FRAME_START_1) {
            // "A5 A5 5A": un A5 repetido puede ser el inicio real, así
            // que solo se reinicia si no es A5
            reset();
        }
        return false;
    }

    // Guardar el byte
    buffer_[count_] = byte;
    count_++;

    // Con 5 bytes ya se conoce el largo de la carga
    if (count_ == 5) {
        uint16_t length = buffer_[3] | (buffer_[4] << 8);

        // largo inválido: se descarta antes de seguir guardando
        if (length > MAX_PAYLOAD) {
            length_errors_++;
            reset();
            return false;
        }

        expected_ = FRAME_MIN_SIZE + length;
    }

    // todavía faltan bytes
    if (count_ < 5 || count_ < expected_) {
        return false;
    }

    // Trama completa: decode_frame valida el CRC
    bool valid = decode_frame(buffer_, count_, frame);

    if (valid) {
        valid_frames_++;
    } else {
        crc_errors_++;
    }

    // Lista para la siguiente trama
    reset();
    return valid;
}
