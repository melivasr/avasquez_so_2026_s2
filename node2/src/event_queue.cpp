#include "event_queue.h"
//Implementacion de cola circular de eventos.

EventQueue::EventQueue(){
    head_ = 0;
    tail_ = 0;
    count_ = 0;
}

bool EventQueue::push(const NodeEvent &item){
    if (full()) {
        return false;
    }

    buffer_[head_] = item;
    head_ = (head_ + 1) % EVENT_QUEUE_CAPACITY;
    ++count_;

    return true;
}

bool EventQueue::pop(NodeEvent &item){
    if (empty()) {
        return false;
    }

    item = buffer_[tail_];
    tail_ = (tail_ + 1) % EVENT_QUEUE_CAPACITY;
    --count_;

    return true;
}

bool EventQueue::empty() const{
    return count_ == 0;
}

bool EventQueue::full() const{
    return count_ == EVENT_QUEUE_CAPACITY;
}

size_t EventQueue::size() const{
    return count_;
}

size_t EventQueue::capacity() const{
    return EVENT_QUEUE_CAPACITY;
}
