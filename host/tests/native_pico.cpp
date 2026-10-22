/* Adaptador de prueba: ejecuta los .cpp reales de Pico en Linux.
 * Solo sustituye GPIO, reloj/alarma y Serial. No valida el SDK ni el hardware. */
#include "Arduino.h"
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
NativeSerial Serial;
using Clock=std::chrono::steady_clock;
static const auto epoch=Clock::now();
static uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-epoch).count();
}
uint32_t micros() { return (uint32_t)now_us(); }
uint32_t millis() { return (uint32_t)(now_us()/1000); }
uint32_t time_us_32() { return micros(); }
static bool (*alarm_callback)(repeating_timer_t *)=nullptr;
static repeating_timer_t *alarm_timer=nullptr;
static uint64_t next_alarm=0,alarm_period=0;
static int tx_space=64;
bool add_repeating_timer_us(int64_t period,bool (*callback)(repeating_timer_t *),
                           void *,repeating_timer_t *timer) {
    alarm_period=(uint64_t)(period<0?-period:period);
    next_alarm=now_us()+alarm_period;
    alarm_callback=callback; alarm_timer=timer;
    return alarm_period!=0;
}
bool cancel_repeating_timer(repeating_timer_t *) { alarm_callback=nullptr; return true; }
void pinMode(uint8_t,int) {}
void digitalWrite(uint8_t,int) {}
int digitalRead(uint8_t) { return 1; }
int digitalPinToInterrupt(uint8_t pin) { return pin; }
void attachInterrupt(int,void (*)(),int) {}
int NativeSerial::available() {
    int bytes=0; return ioctl(STDIN_FILENO,FIONREAD,&bytes)==0?bytes:0;
}
int NativeSerial::availableForWrite() {
    pollfd p={STDOUT_FILENO,POLLOUT,0};
    return poll(&p,1,0)>0 && (p.revents&POLLOUT)?tx_space:0;
}
int NativeSerial::read() {
    uint8_t byte; return ::read(STDIN_FILENO,&byte,1)==1?byte:-1;
}
size_t NativeSerial::write(const uint8_t *p,size_t size) {
    ssize_t result=::write(STDOUT_FILENO,p,size);
    if(result>0)tx_space-=(int)result;
    return result>0?(size_t)result:0;
}
void setup(); void loop();
int main() {
    fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO,F_GETFL)|O_NONBLOCK);
    fcntl(STDOUT_FILENO,F_SETFL,fcntl(STDOUT_FILENO,F_GETFL)|O_NONBLOCK);
    setup();
    while(true) {
        const uint64_t now=now_us();
        while(alarm_callback && now>=next_alarm) {
            next_alarm+=alarm_period;
            alarm_callback(alarm_timer);
        }
        tx_space=64;
        loop();
        poll(nullptr,0,1);
    }
}
