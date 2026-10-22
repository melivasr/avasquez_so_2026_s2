#include "host.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
int serial_open(const char *path) {
    int fd=open(path,O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)return -1;
    struct termios t;
    if(ioctl(fd,TIOCEXCL)<0 || tcgetattr(fd,&t)<0) goto bad;
    cfmakeraw(&t);
    cfsetispeed(&t,B115200);
    cfsetospeed(&t,B115200);
    t.c_cflag|=CLOCAL|CREAD;
    t.c_cflag&=~(CSTOPB|CRTSCTS);
    t.c_cc[VMIN]=1;
    t.c_cc[VTIME]=0;
    if(tcsetattr(fd,TCSANOW,&t)<0)goto bad;
    /* Evita bajar DTR deliberadamente al cerrar; la placa puede tener auto-reset propio. */
    t.c_cflag&=~(tcflag_t)HUPCL;
    if(tcsetattr(fd,TCSANOW,&t)<0)goto bad;
    return fd;
    bad: {
        int e=errno;
        close(fd);
        errno=e;
        return -1;
    }
}

int serial_write(int fd,const uint8_t *p,size_t n,uint32_t ms) {
    uint64_t end=monotonic_ns()+(uint64_t)ms*1000000;
    while(n) {
        uint64_t now=monotonic_ns();
        if(now>=end) {
            errno=ETIMEDOUT;
            return -1;
        }
        int wait=(int)((end-now+999999)/1000000);
        struct pollfd f= {
            .fd=fd,.events=POLLOUT
        };
        int r=poll(&f,1,wait);
        if(r<0 && errno==EINTR)continue;
        if(r<=0 || (f.revents&(POLLERR|POLLHUP|POLLNVAL)))return -1;
        ssize_t k=write(fd,p,n);
        if(k<0 && (errno==EINTR || errno==EAGAIN))continue;
        if(k<=0)return -1;
        p+=(size_t)k;
        n-=(size_t)k;
    }
    return 0;
}
