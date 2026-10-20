#include "host.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e=s+strlen(s);
    while(e>s && isspace((unsigned char)e[-1])) *--e=0;
    return s;
}

bool config_load(const char *path,Config *c,char *err,size_t n) {
    *c=(Config) {
        .algorithm=EDF,.heuristic=FIRST_FIT,.tick_us=10000,.report_ticks=10,.missed_reports=3,.timeout_ms=2000
    };
    snprintf(c->output,sizeof c->output,"report");
    FILE *f=fopen(path,"r");
    if (!f) {
        snprintf(err,n,"%s: %s",path,strerror(errno));
        return false;
    }
    char line[2049];
    size_t ln=0;
    bool ok=true;
    unsigned seen=0;
    int read_result;
    const char *keys[]= {
        "algorithm","heuristic","tick_us","esp_port","pico_port","report_ticks","missed_reports","tasks","output","timeout_ms"
    };
    while ((read_result=bounded_line(f,line,sizeof line))>0) {
        ln++;
        char *s=trim(line);
        if (!*s || *s=='#') continue;
        char *eq=strchr(s,'=');
        if (!eq) {
            ok=false;
            break;
        }
        *eq=0;
        char *key=trim(s),*v=trim(eq+1);
        int k=-1;
        for(int i=0;i<10;i++) if(!strcmp(key,keys[i])) k=i;
        if(k<0 || (seen&(1u<<k)) || !*v) {
            ok=false;
            break;
        }
        seen|=1u<<k;
        uint32_t x=0;
        if (k==0) {
            if(!strcmp(v,"EDF")) c->algorithm=EDF;
            else if(!strcmp(v,"RMS")) c->algorithm=RMS;
            else ok=false;
        } else if(k==1) {
            if(!strcmp(v,"First-Fit")) c->heuristic=FIRST_FIT;
            else if(!strcmp(v,"Worst-Fit")) c->heuristic=WORST_FIT;
            else ok=false;
        } else if(k==3 || k==4 || k==7 || k==8) {
            char *dst=k==3?c->ports[0]:k==4?c->ports[1]:k==7?c->tasks:c->output;
            size_t sz=(k==3 || k==4)?sizeof c->ports[0]:sizeof c->tasks;
            if(strlen(v)>=sz) ok=false;
            else memcpy(dst,v,strlen(v)+1);
        } else {
            if(!parse_u32(v,&x) || !x) ok=false;
            else if(k==2) {
                c->tick_us=x;
                if(x<1000 || x>1000000) ok=false;
            } else if(k==5) c->report_ticks=x;
            else if(k==6) c->missed_reports=x;
            else {
                c->timeout_ms=x;
                if(x<100 || x>60000) ok=false;
            }
        }
        if(!ok) break;
    }
    if(read_result<0)ok=false;
    if(!*c->ports[0] || !*c->ports[1] || !*c->tasks || !strcmp(c->ports[0],c->ports[1])) ok=false;
    long double window=(long double)c->tick_us*c->report_ticks*c->missed_reports;
    if(window>86400000000.0L) ok=false;
    if(!ok) snprintf(err,n,"configuración inválida cerca de línea %zu (clave desconocida/duplicada, rango o puerto/ruta faltante; vigilancia máxima 24 h)",ln);
    fclose(f);
    return ok;
}
