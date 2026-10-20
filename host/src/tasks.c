#include "host.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
/* Lectura acotada: una línea maliciosa no provoca una asignación enorme. */
int bounded_line(FILE *f,char *out,size_t capacity) {
    size_t used=0;
    int ch;
    while((ch=fgetc(f))!=EOF) {
        if(ch==0 || used+1>=capacity) {
            errno=EINVAL;
            return -1;
        }
        out[used++]=(char)ch;
        if(ch=='\n')break;
    }
    out[used]=0;
    if(ferror(f))return -1;
    return used?1:0;
}

bool parse_u32(const char *s, uint32_t *v) {
    if (!s || !*s) return false;
    for (const char *p=s; *p; ++p) if (!isdigit((unsigned char)*p)) return false;
    errno=0;
    char *end;
    unsigned long long x=strtoull(s,&end,10);
    if (errno || *end || x>UINT32_MAX) return false;
    *v=(uint32_t)x;
    return true;
}

static bool fail(char *e,size_t n,const char *s) {
    snprintf(e,n,"%s",s);
    return false;
}

bool task_parse(const char *line, Task *out, char *err, size_t n) {
    char b[160];
    size_t len=strcspn(line,"\r\n");
    if (line[len] && strspn(line+len,"\r\n")!=strlen(line+len)) return fail(err,n,"contenido después del fin de línea");
    if (!len || len>=sizeof b) return fail(err,n,"línea vacía o demasiado larga");
    memcpy(b,line,len);
    b[len]=0;
    char *f[6];
    f[0]=b;
    size_t k=1;
    for (size_t i=0;i<len;i++) if (b[i]==',') {
        if (k==6) return fail(err,n,"se requieren exactamente seis campos");
        b[i]=0;
        f[k++]=b+i+1;
    }
    if (k!=6) return fail(err,n,"se requieren exactamente seis campos");
    Task t= {
        0
    };
    if (!parse_u32(f[0],&t.id) || !t.id || t.id==NO_TASK || !parse_u32(f[2],&t.W) || !t.W || !parse_u32(f[3],&t.T) || !t.T || !parse_u32(f[4],&t.D) || !t.D) return fail(err,n,"id [1,4294967294]; W/T/D positivos uint32");
    if (strlen(f[1])!=1 || (f[1][0]!='P' && f[1][0]!='S')) return fail(err,n,"tipo debe ser P o S");
    t.type=f[1][0];
    const char *p=f[5];
    if (*p=='-') p++;
    if (!*p) return fail(err,n,"criticidad debe ser int32");
    for (;*p;p++) if (!isdigit((unsigned char)*p)) return fail(err,n,"criticidad debe ser int32");
    errno=0;
    char *end;
    long long c=strtoll(f[5],&end,10);
    if (errno || *end || c<INT32_MIN || c>INT32_MAX) return fail(err,n,"criticidad fuera de int32");
    t.criticality=(int32_t)c;
    *out=t;
    return true;
}

bool task_load(const char *path,Task *tasks,size_t *count,char *err,size_t n) {
    FILE *f=fopen(path,"r");
    if (!f) {
        snprintf(err,n,"%s: %s",path,strerror(errno));
        return false;
    }
    char line[160];
    size_t lineno=0;
    int read_result;
    *count=0;
    bool ok=true;
    while ((read_result=bounded_line(f,line,sizeof line))>0) {
        lineno++;
        if (line[0]=='#' || line[0]=='\n' || line[0]=='\r') continue;
        Task t;
        char why[160];
        if (*count==TASK_CAP || !task_parse(line,&t,why,sizeof why)) {
            snprintf(err,n,"línea %zu: %s",lineno,*count==TASK_CAP?"máximo 512 tareas":why);
            ok=false;
            break;
        }
        for (size_t i=0;i<*count;i++) if (tasks[i].id==t.id) {
            snprintf(err,n,"línea %zu: id duplicado %"PRIu32,lineno,t.id);
            ok=false;
            break;
        }
        if (!ok) break;
        tasks[(*count)++]=t;
    }
    if(read_result<0) {
        snprintf(err,n,"línea %zu: lectura inválida (máximo 158 caracteres, sin NUL): %s",lineno+1,strerror(errno));
        ok=false;
    }
    fclose(f);
    return ok;
}
