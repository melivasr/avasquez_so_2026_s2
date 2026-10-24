#include "host.h"
#include <math.h>
#include <string.h>
uint32_t task_cost(uint32_t w,uint32_t speed) {
    return speed?w/speed+(w%speed!=0):0;
}

long double node_util(const Node *n) {
    long double u=0;
    for(size_t i=0;i<n->count;i++) u+=(long double)task_cost(n->tasks[i].W,n->speed)/n->tasks[i].T;
    return u;
}
/* EDF exacto: hasta ocho denominadores uint32; nueve limbs evitan
* redondear a 1 una suma racional apenas superior a 1. */
#define LIMBS 9
static void big_mul(uint32_t x[LIMBS],uint32_t m) {
    uint64_t carry=0;
    for(size_t i=0;i<LIMBS;i++) {
        uint64_t v=(uint64_t)x[i]*m+carry;
        x[i]=(uint32_t)v;
        carry=v>>32;
    }
}

static void big_add(uint32_t x[LIMBS],const uint32_t y[LIMBS]) {
    uint64_t carry=0;
    for(size_t i=0;i<LIMBS;i++) {
        uint64_t v=(uint64_t)x[i]+y[i]+carry;
        x[i]=(uint32_t)v;
        carry=v>>32;
    }
}

static bool edf_exact(const Node *node,const Task *candidate) {
    Task tasks[NODE_CAP];
    memcpy(tasks,node->tasks,node->count*sizeof *tasks);
    tasks[node->count]=*candidate;
    size_t n=node->count+1;
    uint32_t denominator[LIMBS]= {
        1
    },sum[LIMBS]= {
        0
    };
    for(size_t i=0;i<n;i++)big_mul(denominator,tasks[i].T);
    for(size_t i=0;i<n;i++) {
        uint32_t term[LIMBS]= {
            1
        };
        big_mul(term,task_cost(tasks[i].W,node->speed));
        for(size_t j=0;j<n;j++)if(j!=i)big_mul(term,tasks[j].T);
        big_add(sum,term);
    }
    for(size_t i=LIMBS;i>0;i--)if(sum[i-1]!=denominator[i-1])return sum[i-1]<denominator[i-1];
    return true;
}

const char *admission(const Node *n,const Task *t,Algorithm a) {
    if(!n->operational || !n->speed) return "fallo de comunicación/nodo no operativo";
    if(n->count>=NODE_CAP) return "capacidad máxima de tareas (8)";
    for(size_t i=0;i<n->count;i++) {
        if(n->tasks[i].id==t->id) return "id duplicado";
        if(t->type=='S' && n->tasks[i].type=='S') return "máximo una esporádica por nodo";
    }
    long double u=node_util(n)+(long double)task_cost(t->W,n->speed)/t->T;
    long double count=(long double)(n->count+1);
    long double bound=a==EDF?1:count*expm1l(logl(2)/count);
    if(!(a==EDF?edf_exact(n,t):u<=bound))return "utilización excede límite de admisión";
    uint64_t jobs=rt_job_reservation(t->T,t->D);
    for(size_t i=0;i<n->count;i++)jobs+=rt_job_reservation(n->tasks[i].T,n->tasks[i].D);
    return jobs<=RT_MAX_JOBS?NULL:"capacidad de trabajos pendientes (64 por nodo)";
}

int place(Node nodes[NODES],const Task *t,Algorithm a,Heuristic h,char *err,size_t len) {
    int order[2]= {
        0,1
    };
    if(nodes[1].speed>nodes[0].speed) {
        order[0]=1;
        order[1]=0;
    }
    int best=-1;
    long double free_best=-1;
    const char *why[2];
    for(int k=0;k<NODES;k++) {
        int j=order[k];
        why[j]=admission(&nodes[j],t,a);
        if(why[j]) continue;
        long double remain=1-node_util(&nodes[j])-(long double)task_cost(t->W,nodes[j].speed)/t->T;
        if(best<0 || (h==WORST_FIT && remain>free_best)) {
            best=j;
            free_best=remain;
        }
        if(h==FIRST_FIT) break;
    }
    if(best>=0) nodes[best].tasks[nodes[best].count++]=*t;
    else snprintf(err,len,"ESP32: %s; Pico: %s",why[0],why[1]);
    return best;
}

static bool before(Task a,Task b,uint32_t speed) {
    /* Productos uint32 x uint32 caben exactamente en uint64. */
    uint64_t x=(uint64_t)task_cost(a.W,speed)*b.T,y=(uint64_t)task_cost(b.W,speed)*a.T;
    return x>y || (x==y && a.id<b.id);
}

void sort_tasks(Task *t,size_t n,uint32_t speed) {
    for(size_t i=1;i<n;i++) {
        Task v=t[i];
        size_t j=i;
        while(j && before(v,t[j-1],speed)) {
            t[j]=t[j-1];
            j--;
        }
        t[j]=v;
    }
}

size_t migrate_plan(Node nodes[NODES],int failed,Algorithm a,Heuristic h,Task *lost,bool *keep) {
    size_t n=nodes[failed].count;
    memcpy(lost,nodes[failed].tasks,n*sizeof *lost);
    nodes[failed].operational=false;
    nodes[failed].count=0;
    for(size_t i=0;i<n;i++) keep[i]=lost[i].type!='S';
    Node base[NODES];
    memcpy(base,nodes,sizeof base);
    for(;;) {
        Node trial[NODES];
        memcpy(trial,base,sizeof trial);
        Task candidates[NODE_CAP];
        size_t m=0;
        for(size_t i=0;i<n;i++) if(keep[i]) candidates[m++]=lost[i];
        uint32_t fastest=trial[0].speed>trial[1].speed?trial[0].speed:trial[1].speed;
        sort_tasks(candidates,m,fastest);
        bool fits=true;
        char why[192];
        for(size_t i=0;i<m;i++) if(place(trial,&candidates[i],a,h,why,sizeof why)<0) {
            fits=false;
            break;
        }
        if(fits) {
            memcpy(nodes,trial,sizeof trial);
            break;
        }
        size_t drop=n;
        for(size_t i=0;i<n;i++) if(keep[i] && (drop==n || lost[i].criticality<lost[drop].criticality || (lost[i].criticality==lost[drop].criticality && lost[i].id>lost[drop].id))) drop=i;
        if(drop==n) break;
        keep[drop]=false;
    }
    return n;
}
