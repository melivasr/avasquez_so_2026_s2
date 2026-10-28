// Consume eventos compartidos y genera CSV, estadísticas y el Gantt HTML.
#include "host.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Relaciona cada trabajo con su liberación, número y primera ejecución.
typedef struct {
    uint64_t release;
    uint32_t sequence;
    bool first;
} JobStat;

// Estadísticas por pareja tarea/nodo; jobs conserva los trabajos solapados.
typedef struct {
    uint32_t id;
    int node;
    Task task;
    uint64_t releases,runs,completed,misses,ignored;
    uint64_t first_sum,finish_sum,first_n,finish_n,finish_max;
    bool confirmed;
    uint32_t cost;
    JobStat jobs[RT_MAX_JOBS];
    size_t pending;
} Stat;

static FILE *open_output(const Config *c,const char *name) {
    char path[1100];
    snprintf(path,sizeof path,"%s/%s",c->output,name);
    return fopen(path,"w+");
}

// Duplica comillas internas para conservar una celda CSV válida.
static void csv_text(FILE *f,const char *s) {
    fputc('"',f);
    for(;*s;s++) {
        if(*s=='"')fputc('"',f);
        fputc(*s,f);
    }
    fputc('"',f);
}

// Escapa texto antes de incluirlo en JavaScript, incluido < para no cerrar script.
static void js_text(FILE *f,const char *s) {
    fputc('"',f);
    for(;*s;s++) {
        unsigned char c=(unsigned char)*s;
        if(c=='"' || c=='\\')fprintf(f,"\\%c",c);
        else if(c=='<')fputs("\\u003c",f);
        else if(c<32)fprintf(f,"\\u%04x",c);
        else fputc(c,f);
    }
    fputc('"',f);
}

static Stat *find_stat(Stat *stats,size_t *n,uint32_t id,int node) {
    for(size_t i=0;i<*n;i++)if(stats[i].id==id && stats[i].node==node)return &stats[i];
    if(*n>=TASK_CAP*NODES)return NULL;
    Stat *p=&stats[(*n)++];
    *p=(Stat) {
        .id=id,.node=node
    };
    return p;
}

// La traza conserva el cursor y la tarea seleccionada hasta un nuevo evento.
typedef struct {
    uint64_t cursor;
    uint32_t selected;
    bool valid;
} Trace;

// Reconstruye intervalos conocidos; los huecos inválidos no se rellenan como ejecución.
static void advance_trace(Trace *trace,uint64_t until,int node,Stat *stats,size_t *n,
                          FILE *gantt,uint64_t *busy,bool *incomplete) {
    if(until<=trace->cursor)return;
    if(trace->valid && trace->selected) {
        Stat *st=find_stat(stats,n,trace->selected,node);
        if(st && st->pending) {
            // Los trabajos del mismo ID se atienden en orden FIFO; los ticks se atribuyen al más antiguo.
            JobStat *job=&st->jobs[0];
            fprintf(gantt,"%d,%"PRIu32",%"PRIu64",%"PRIu64"\n",
                    node,st->id,trace->cursor,until);
            uint64_t duration=until-trace->cursor;
            *busy+=duration;
            st->runs+=duration;
            if(!job->first && trace->cursor>=job->release) {
                // Esta latencia parte de RELEASE, no del instante físico en que se presionó el botón.
                st->first_sum+=trace->cursor-job->release;
                st->first_n++;
                job->first=true;
            }
        } else if(!st)*incomplete=true;
    }
    trace->cursor=until;
}

// Plantilla de estructura y estilos; estas cadenas son el contenido del HTML generado.
static const char *const PAGE_HEAD[]= {
    "<!doctype html>\n",
    "<html lang='es'>\n",
    "<head>\n",
    "<meta charset='utf-8'>\n",
    "<meta name='viewport' content='width=device-width,initial-scale=1'>\n",
    "<title>Reporte de ejecución · Calendarizador</title>\n",
    "<style>\n",
    ":root{\n",
    "  color-scheme:light;\n",
    "  --page:#f2f1eb;--surface:#fffef9;--surface2:#f5f4ee;\n",
    "  --ink:#242824;--ink2:#51574f;--muted:#687064;\n",
    "  --grid:#dedfd5;--base:#b9bfb2;--tick-grid:#dae0d4;--tick-major:#a5b39d;\n",
    "  --exec:#73a87a;--crit:#a3342b;--good:#356342;--accent:#a14327;\n",
    "  --good-bg:#e9efe3;--bad-bg:#f7e7df;--mono:ui-monospace,SFMono-Regular,Consolas,'Liberation Mono',monospace;\n",
    "}\n",
    "@media(prefers-color-scheme:dark){:root:not([data-theme='light']){\n",
    "  color-scheme:dark;\n",
    "  --page:#191c19;--surface:#202420;--surface2:#272c26;\n",
    "  --ink:#e8e9df;--ink2:#bdc3b5;--muted:#9ca792;\n",
    "  --grid:#373e33;--base:#59634f;--tick-grid:#384632;--tick-major:#5c7052;\n",
    "  --exec:#77a87b;--crit:#f19883;--good:#afcf9c;--accent:#e9a277;\n",
    "  --good-bg:#303d2b;--bad-bg:#432d26;\n",
    "}}\n",
    ":root[data-theme='dark']{\n",
    "  color-scheme:dark;\n",
    "  --page:#191c19;--surface:#202420;--surface2:#272c26;\n",
    "  --ink:#e8e9df;--ink2:#bdc3b5;--muted:#9ca792;\n",
    "  --grid:#373e33;--base:#59634f;--tick-grid:#384632;--tick-major:#5c7052;\n",
    "  --exec:#77a87b;--crit:#f19883;--good:#afcf9c;--accent:#e9a277;\n",
    "  --good-bg:#303d2b;--bad-bg:#432d26;\n",
    "}\n",
    "*{box-sizing:border-box}\n",
    "html{scroll-behavior:smooth;scroll-padding-top:88px}\n",
    "body{margin:0;background:var(--page);color:var(--ink);font:14px/1.65 system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;-webkit-font-smoothing:antialiased}\n",
    "main{max-width:1480px;margin:0 auto;padding:44px 48px 40px;counter-reset:report-section}\n",
    "a{color:var(--accent);text-underline-offset:4px}\n",
    ":focus-visible{outline:2px solid var(--accent);outline-offset:4px}\n",
    "/* Reloj y paquete entre nodos: decoración, independiente de los datos del registro. */\n",
    ".report-header{position:relative;display:flex;align-items:flex-end;justify-content:space-between;gap:28px;padding:46px 0 30px;border-top:2px solid var(--ink)}\n",
    ".report-header::before{content:'';position:absolute;top:14px;left:0;width:144px;height:16px;background:var(--muted);clip-path:polygon(0 75%,7% 75%,7% 0,20% 0,20% 75%,33% 75%,33% 0,46% 0,46% 75%,59% 75%,59% 0,72% 0,72% 75%,85% 75%,85% 0,98% 0,98% 100%,97% 100%,97% 12%,86% 12%,86% 87%,71% 87%,71% 12%,60% 12%,60% 87%,45% 87%,45% 12%,34% 12%,34% 87%,19% 87%,19% 12%,8% 12%,8% 87%,0 87%)}\n",
    ".report-header::after{content:'';position:absolute;top:34px;left:0;width:5px;height:3px;background:var(--accent);animation:tick-packet 5s steps(24,end) infinite}\n",
    "@keyframes tick-packet{0%,12%{transform:translateX(0);opacity:0}16%{opacity:1}80%{transform:translateX(139px);opacity:1}86%,100%{transform:translateX(139px);opacity:0}}\n",
    "h1{margin:0 0 10px;font-size:clamp(30px,3.5vw,46px);font-weight:600;line-height:1.12;letter-spacing:-.055em}\n",
    ".report-header>div{min-width:0;max-width:100%}\n",
    ".lead{margin:0;font:12px/1.7 var(--mono);color:var(--ink2)}\n",
    ".downloads{display:flex;flex-wrap:wrap;gap:8px 22px;flex-shrink:0;max-width:100%;padding-bottom:3px}\n",
    ".downloads a{display:inline-flex;align-items:center;gap:8px;padding:5px 0;border-bottom:1px solid var(--base);font-size:12px;font-weight:500;color:var(--ink2);text-decoration:none;transition:color .15s,border-color .15s}\n",
    ".downloads a:hover{color:var(--accent);border-color:var(--accent)}\n",
    ".downloads span{font:10px/1.5 var(--mono);color:var(--muted)}\n",
    ".summary{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));padding:22px 0 24px;border-top:1px solid var(--base);border-bottom:1px solid var(--base)}\n",
    ".metric{padding:0 24px;border-left:1px solid var(--grid)}\n",
    ".metric:first-child{padding-left:0;border-left:0}\n",
    ".metric-label{margin:0 0 12px;font-size:12px;font-weight:500;color:var(--ink2)}\n",
    ".metric-value{display:block;font:400 clamp(30px,3vw,42px)/1.1 var(--mono);letter-spacing:-.065em;font-variant-numeric:tabular-nums}\n",
    ".metric-note{display:block;margin-top:12px;font:10px/1.7 var(--mono);color:var(--muted)}\n",
    ".metric.alert .metric-value{color:var(--crit)}\n",
    ".report-nav{position:sticky;top:0;z-index:10;display:flex;gap:28px;overflow-x:auto;padding:0 2px;border-bottom:1px solid var(--base);background:var(--page);scrollbar-width:thin}\n",
    ".report-nav a{flex-shrink:0;padding:17px 0 13px;border-bottom:2px solid transparent;font:11px/1.5 var(--mono);color:var(--ink2);text-decoration:none;transition:color .15s,border-color .15s}\n",
    ".report-nav a:hover,.report-nav a:focus-visible{border-bottom-color:var(--accent);color:var(--accent)}\n",
    ".report-section{margin-top:36px;min-width:0;counter-increment:report-section}\n",
    ".section-heading{display:flex;align-items:flex-start;gap:14px;margin-bottom:18px}\n",
    ".section-heading::before{content:counter(report-section,decimal-leading-zero);flex-shrink:0;min-width:28px;padding-top:4px;font:11px/1.6 var(--mono);color:var(--accent)}\n",
    "h2{margin:0;font-size:20px;font-weight:550;letter-spacing:-.025em;line-height:1.4}\n",
    ".section-heading p{margin:3px 0 0;color:var(--muted);font-size:12px}\n",
    "h3{margin:0;font:600 15px/1.5 var(--mono);letter-spacing:-.025em}\n",
    "p{color:var(--ink2)}.muted{color:var(--muted)}\n",
    ".section-note{max-width:115ch;margin:15px 0 0;padding-left:42px;color:var(--muted);font-size:12px;line-height:1.8}\n",
    ".card{overflow:hidden;background:var(--surface);border:1px solid var(--base);border-radius:2px}\n",
    ".node-card{padding:22px 24px 18px;margin-bottom:18px;border-top:2px solid var(--ink2)}\n",
    ".node-head{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:20px}\n",
    ".node-meta{margin:4px 0 0;color:var(--muted);font:11px/1.6 var(--mono)}\n",
    ".ctl{display:flex;flex-wrap:wrap;align-items:center;gap:12px;padding:10px 0 13px;margin-bottom:12px;border-top:1px solid var(--grid);border-bottom:1px solid var(--grid)}\n",
    ".pager{display:flex;gap:4px}\n",
    ".control-field{display:flex;align-items:center;gap:8px;white-space:nowrap;color:var(--ink2);font-size:12px}\n",
    "button,select,input{min-height:34px;border:1px solid var(--base);border-radius:2px;background:var(--surface);color:var(--ink);font:12px/1.5 var(--mono)}\n",
    "button,select{padding:5px 10px}button{cursor:pointer;transition:background .15s,border-color .15s}\n",
    "button:hover{border-color:var(--accent);background:var(--surface2)}\n",
    ".icon-button{width:34px;height:34px;padding:0;font:17px/1 system-ui,sans-serif}\n",
    ".ctl input[type=number]{width:78px;padding:4px 8px;font-variant-numeric:tabular-nums}\n",
    ".ctl input[type=range]{flex:1;min-width:100px;min-height:24px;border:0;background:transparent;accent-color:var(--accent)}\n",
    ".event-toggle{display:flex;align-items:center;gap:7px;white-space:nowrap;color:var(--ink2);font-size:12px;cursor:pointer}\n",
    ".event-toggle input{width:14px;height:14px;min-height:0;margin:0;accent-color:var(--accent)}\n",
    ".replay{display:flex;align-items:center;flex-wrap:wrap;gap:7px;margin:0 0 18px}\n",
    ".replay button,.replay select{min-height:32px;font-size:11px}\n",
    ".replay-button{display:inline-flex;align-items:center;justify-content:center;gap:8px;min-width:112px;background:var(--ink);border-color:var(--ink);color:var(--surface)}\n",
    ".replay-button:hover{background:var(--ink2);border-color:var(--ink2)}\n",
    ".replay-button::before{content:'▶';font-size:9px}.replay-button[aria-pressed='true']::before{content:'Ⅱ';font-size:12px}\n",
    ".replay-readout{margin-left:auto;color:var(--muted);font:11px/1.7 var(--mono);font-variant-numeric:tabular-nums}\n",
    ".replay button:disabled,.replay select:disabled{opacity:.45;cursor:default}\n",
    ".legend{display:flex;flex-wrap:wrap;gap:8px 20px;margin:0 0 10px;color:var(--ink2);font-size:11px}\n",
    ".legend>span{display:inline-flex;align-items:center;gap:6px}.legend svg{flex-shrink:0}\n",
    ".chart-info{display:block;margin-bottom:12px;color:var(--muted);font:11px/1.7 var(--mono)}\n",
    ".chart{width:100%;overflow-x:auto;padding:12px 0 2px;border-top:1px solid var(--grid);scrollbar-width:thin}\n",
    ".chart>svg{display:block}.chart text{fill:var(--muted);font:11px/1.5 var(--mono);font-variant-numeric:tabular-nums}\n",
    ".chart text.lane{fill:var(--ink);font-weight:600}\n",
    ".grid{stroke:var(--tick-grid);stroke-width:1;shape-rendering:crispEdges;pointer-events:none}.grid.major{stroke:var(--tick-major)}\n",
    ".execution{fill:var(--exec)}.rel{fill:var(--ink)}.dl{fill:none;stroke:var(--ink2);stroke-width:1.5}\n",
    ".ign{fill:var(--surface);stroke:var(--ink2);stroke-width:1.5}\n",
    ".miss path{stroke:var(--crit);stroke-width:2.5;stroke-linecap:round}.miss path.ring{stroke:var(--surface);stroke-width:6}\n",
    ".playhead{pointer-events:none}.playhead line{stroke:var(--accent);stroke-width:1.5;stroke-dasharray:3 3}.playhead circle{fill:var(--accent)}\n",
    ".replay-lane{fill:var(--accent);opacity:.08}.replay-cell{fill:none;stroke:var(--ink);stroke-width:2}\n",
    ".table-wrap{width:100%;overflow-x:auto;scrollbar-width:thin}\n",
    "table{width:100%;border-collapse:separate;border-spacing:0;font-size:12px;font-variant-numeric:tabular-nums}\n",
    "th,td{padding:13px 15px;border-bottom:1px solid var(--grid);text-align:right;white-space:nowrap}\n",
    "th{min-width:65px;max-width:140px;background:var(--surface2);color:var(--ink2);white-space:normal;vertical-align:bottom;font:500 10px/1.65 var(--mono)}\n",
    "td{color:var(--ink2)}tbody tr:nth-child(even) td{background:var(--surface2)}tbody tr:hover td{background:var(--good-bg)}tbody tr:last-child td{border-bottom:0}\n",
    "th:first-child,td:first-child{position:sticky;left:0;z-index:1;background:var(--surface);color:var(--ink);text-align:left;font-family:var(--mono);font-weight:600}\n",
    "th:first-child{background:var(--surface2)}td.l,th.l{text-align:left}\n",
    "td.why{min-width:22em;white-space:normal;text-align:left;line-height:1.7}\n",
    ".card>p{max-width:115ch;margin:0;padding:17px 20px;font-size:12px;line-height:1.8}.card>.table-wrap+p{border-top:1px solid var(--grid)}\n",
    "#util td:last-child{min-width:280px;max-width:520px;white-space:normal;line-height:1.8}\n",
    ".status{display:inline-flex;align-items:center;gap:5px;padding:3px 8px;border:1px solid var(--base);border-radius:2px;font:500 10px/1.6 var(--mono);white-space:nowrap}\n",
    ".status.ok{background:var(--good-bg);border-color:transparent}.status.bad{background:var(--bad-bg);border-color:transparent}.status.neutral{background:var(--surface2);color:var(--ink2)}\n",
    ".bad{color:var(--crit);font-weight:600}.ok{color:var(--good);font-weight:600}\n",
    "details{border-top:1px solid var(--grid)}details summary{padding:15px 20px;cursor:pointer;color:var(--ink2);font:12px/1.6 var(--mono)}\n",
    "details[open] summary{border-bottom:1px solid var(--grid)}details summary:hover{color:var(--accent)}\n",
    "#tip{position:fixed;z-index:30;max-width:min(360px,calc(100vw - 16px));padding:8px 11px;border:1px solid var(--base);border-radius:2px;background:var(--surface);color:var(--ink);pointer-events:none;font:11px/1.6 var(--mono);box-shadow:0 3px 10px #0002}\n",
    ".report-footer{display:flex;justify-content:space-between;gap:20px;margin-top:40px;padding-top:18px;border-top:2px solid var(--ink);color:var(--muted);font:10px/1.8 var(--mono)}\n",
    ".report-footer a{flex-shrink:0;color:var(--ink2);text-decoration:none}.report-footer a:hover{color:var(--accent)}\n",
    "@media(max-width:1050px){main{padding:32px 28px}.report-header{align-items:flex-start;flex-direction:column;gap:20px}.metric{padding-right:14px;padding-left:18px}.ctl input[type=range]{flex-basis:140px}.replay-readout{font-size:10px}}\n",
    "@media(max-width:640px){\n",
    "  main{padding:22px 16px 32px}html{scroll-padding-top:74px}\n",
    "  .report-header{padding:45px 0 24px}.lead{font-size:11px}.downloads{gap:8px 20px}\n",
    "  .summary{grid-template-columns:repeat(2,minmax(0,1fr));padding:0}\n",
    "  .metric,.metric:first-child{padding:18px 12px 18px 0;border-left:0}.metric:nth-child(even){padding-left:16px;border-left:1px solid var(--grid)}\n",
    "  .metric:nth-child(-n+2){border-bottom:1px solid var(--grid)}.metric-label{min-height:20px;margin-bottom:8px;font-size:11px}.metric-value{font-size:32px}.metric-note{margin-top:9px;font-size:10px}\n",
    "  .report-nav{gap:22px}.report-nav a{padding-top:15px;padding-bottom:12px}\n",
    "  .report-section{margin-top:28px}.section-heading{gap:8px}.section-heading::before{min-width:24px}h2{font-size:18px}.section-note{padding-left:0}\n",
    "  .node-card{padding:16px 12px 12px}.node-head{align-items:flex-start;gap:8px}.node-meta{font-size:10px}.node-head>.status{padding:3px 5px;font-size:9px}\n",
    "  .ctl{gap:10px 8px}.pager{flex-basis:100%}.control-field{font-size:11px}.ctl input[type=number]{width:66px}.ctl input[type=range]{flex-basis:100%;order:1}.event-toggle{font-size:11px}\n",
    "  .replay-readout{flex-basis:100%;margin:4px 0 0}.legend{gap:7px 14px;font-size:10px}.chart-info{font-size:10px}\n",
    "  th,td{padding:11px 12px}.report-footer{flex-direction:column;gap:8px}\n",
    "}\n",
    "@media(prefers-reduced-motion:reduce){html{scroll-behavior:auto}*,*::before,*::after{animation:none!important;transition:none!important}.report-header::after{opacity:0}}\n",
    "@media print{\n",
    "  :root,:root[data-theme]{color-scheme:light;--page:#fff;--surface:#fff;--surface2:#f5f5f2;--ink:#202420;--ink2:#454b42;--muted:#5b6355;--grid:#ddd;--base:#aaa;--tick-grid:#ddd;--tick-major:#aaa;--exec:#73a87a;--crit:#a3342b;--good:#356342;--accent:#a14327;--good-bg:#e9efe3;--bad-bg:#f7e7df}\n",
    "  body{background:#fff}main{max-width:none;padding:0}.report-header{padding-top:16px}.report-header::before,.report-header::after,.report-nav,.downloads,.ctl,.replay,.report-footer>a{display:none}\n",
    "  .playhead{display:none!important}.node-card{break-inside:avoid}.chart>svg{max-width:100%;height:auto}.report-section{margin-top:24px}.table-wrap{overflow:visible}th:first-child,td:first-child{position:static}\n",
    "}\n",
    "</style>\n",
    "</head>\n",
    "<body>\n",
    "<main id='top'>\n",
    "<header class='report-header'>\n",
    "<div><h1>Reporte de ejecución</h1><p class='lead'>ESP32 y Raspberry Pi Pico · Sistema heterogéneo</p></div>\n",
    "<div class='downloads' aria-label='Exportar datos'><a href='events.csv' download>Eventos <span>CSV ↓</span></a><a href='gantt.csv' download>Intervalos <span>CSV ↓</span></a><a href='tasks.csv' download>Estadísticas <span>CSV ↓</span></a></div>\n",
    "</header>\n",
    "<div class='summary' id='summary' aria-label='Resumen de la ejecución'></div>\n",
    "<nav class='report-nav' aria-label='Secciones del reporte'><a href='#timeline'>Gantt</a><a href='#utilization'>Utilización</a><a href='#task-stats'>Tareas</a><a href='#sporadic'>Esporádicas</a><a href='#admission'>Admisión</a></nav>\n",
    "<section class='report-section' id='timeline'>\n",
    "<div class='section-heading'><div><h2>Diagrama de Gantt por nodo</h2><p>Intervalos de ejecución y plazos por tarea.</p></div></div>\n",
    "<div id='gantt'></div>\n",
    "<p class='section-note'>Una fila por tarea y una columna por tick en la vista de detalle. El Gantt se reconstruye a partir de los eventos recibidos; cada nodo usa su propio reloj local. Los intervalos con pérdidas se consideran incompletos y los espacios vacíos no prueban inactividad. Reproducir recorre este registro a la velocidad elegida.</p>\n",
    "</section>\n",
    "<section class='report-section' id='utilization'>\n",
    "<div class='section-heading'><div><h2>Utilización y cambios de tarea por nodo</h2><p>Carga admitida y ejecución observada.</p></div></div>\n",
    "<div class='card scroll' id='util'></div>\n",
    "</section>\n",
    "<section class='report-section' id='task-stats'>\n",
    "<div class='section-heading'><div><h2>Estadísticas por tarea</h2><p>Costos, trabajos completados y tiempos de respuesta.</p></div></div>\n",
    "<div class='card scroll' id='stats'></div>\n",
    "</section>\n",
    "<section class='report-section' id='sporadic'>\n",
    "<div class='section-heading'><div><h2>Esporádicas y pulsaciones del botón</h2><p>Liberaciones aceptadas, solicitudes ignoradas y latencias.</p></div></div>\n",
    "<div class='card scroll' id='presses'></div>\n",
    "</section>\n",
    "<section class='report-section' id='admission'>\n",
    "<div class='section-heading'><div><h2>Tareas aceptadas y rechazadas</h2><p>Estado final de las tareas e historial de decisiones.</p></div></div>\n",
    "<div class='card scroll' id='admit'></div>\n",
    "</section>\n",
    "<footer class='report-footer'><span>Calendarizador heterogéneo · Tiempos expresados en ticks de cada nodo</span><a href='#top'>Volver al inicio ↑</a></footer>\n",
    "<div id='tip' hidden></div>\n",
    "</main>\n",
};

// Visualiza datos ya registrados; reproducir el Gantt no vuelve a calendarizar tareas.
static const char *const PAGE_SCRIPT[]= {
    "(function(){\n",
    "'use strict';\n",
    "const STATE={0:['…','Admitida (pendiente)',''],1:['✓','Aceptada','ok'],2:['↺','Retirada','bad'],3:['✕','Rechazada','bad'],4:['?','Incierta','bad'],5:['✕','Intento de alta rechazado','bad']};\n",
    "const SPANS=[20,50,100,200,500,1000,5000,0];\n",
    "const DENSE=1500;\n",
    "const NS='http://www.w3.org/2000/svg';\n",
    "function S(t,a,p){const e=document.createElementNS(NS,t);for(const k in a)e.setAttribute(k,a[k]);if(p)p.appendChild(e);return e;}\n",
    "function H(t,c,txt){const e=document.createElement(t);if(c)e.className=c;if(txt!=null)e.textContent=txt;return e;}\n",
    "const last=new Map();for(const d of R.dec)if(d.k!==5)last.set(d.id,d);\n",
    "const finals=[...last.values()].sort(function(p,q){return p.id-q.id;});\n",
    "const nAcc=finals.filter(function(d){return d.k===1;}).length,nRej=finals.filter(function(d){return d.k===3;}).length;\n",
    "const completed=R.tasks.reduce((sum,t)=>sum+t.comp,0),missed=R.tasks.reduce((sum,t)=>sum+t.miss,0);\n",
    "const metrics=[['Tareas aceptadas',nAcc,nRej+' rechazadas · '+finals.length+' recibidas'],['Trabajos completados',completed,'Finalizaciones registradas'],['Fallas de plazo',missed,'Eventos MISS registrados'],['Cambios de tarea',R.util.reduce((sum,u)=>sum+u.sw,0),'Entradas a tareas distintas']];\n",
    "for(let i=0;i<metrics.length;i++){const m=metrics[i],card=H('div','metric'+(i===2&&missed?' alert':''));card.append(H('p','metric-label',m[0]),H('strong','metric-value',m[1]),H('span','metric-note',m[2]));document.getElementById('summary').appendChild(card);}\n",
    "const tip=document.getElementById('tip');\n",
    "document.addEventListener('mousemove',function(ev){\n",
    "  const t=ev.target.closest&&ev.target.closest('[data-tip]');\n",
    "  if(!t){tip.hidden=true;return;}\n",
    "  tip.textContent=t.getAttribute('data-tip');tip.hidden=false;\n",
    "  const r=tip.getBoundingClientRect();let x=ev.clientX+14,y=ev.clientY+14;\n",
    "  if(x+r.width>innerWidth-8)x=ev.clientX-r.width-14;if(y+r.height>innerHeight-8)y=ev.clientY-r.height-14;\n",
    "  tip.style.left=x+'px';tip.style.top=y+'px';\n",
    "});\n",
    "function niceStep(span){const raw=span/8,p=Math.pow(10,Math.floor(Math.log10(raw)));for(const m of [1,2,5,10])if(m*p>=raw)return Math.max(1,m*p);return 10*p;}\n",
    "function fixed(v,d){return v==null?'—':v.toFixed(d);}\n",
    "function table(cols,rows){\n",
    "  const t=H('table'),hr=H('tr');\n",
    "  for(const c of cols){const th=H('th',c[1]||'',c[0]);hr.appendChild(th);}\n",
    "  t.appendChild(H('thead')).appendChild(hr);const b=t.appendChild(H('tbody'));\n",
    "  for(const r of rows){const tr=b.appendChild(H('tr'));r.forEach(function(v,i){const td=tr.appendChild(H('td',cols[i][1]||''));if(v instanceof Node)td.appendChild(v);else td.textContent=v;});}\n",
    "  const wrap=H('div','table-wrap');wrap.appendChild(t);return wrap;\n",
    "}\n",
    "function badge(code){const s=STATE[code]||['?','Código '+code,''];return H('span','status '+(s[2]||'neutral'),s[0]+' '+s[1]);}\n",
    "function legendItem(svg,label){const s=H('span');s.innerHTML=svg;s.appendChild(document.createTextNode(label));return s;}\n",
    "\n",
    "function nodeChart(n,root){\n",
    "  const tasks=R.tasks.filter(t=>t.n===n),byId=new Map(tasks.map(t=>[t.id,t]));\n",
    "  const iv=R.iv[n],mk=R.mk[n],ids=new Set(tasks.map(t=>t.id));\n",
    "  for(let i=0;i<iv.length;i+=3)ids.add(iv[i]);\n",
    "  const lanes=[...ids].sort((a,b)=>a-b);\n",
    "  for(let i=0;i<mk.length;i+=4)if(mk[i]===5&&mk[i+1]===0){lanes.push(0);break;}\n",
    "  const lane=new Map(lanes.map((id,i)=>[id,i])),end=Math.max(R.util[n].end,1);\n",
    "  const sec=root.appendChild(H('section','card node-card'));\n",
    "  const head=sec.appendChild(H('div','node-head')),name=H('div');\n",
    "  name.append(H('h3',null,R.nodes[n]),H('p','node-meta',tasks.length+' tarea(s) · '+R.util[n].end+' ticks observados'));\n",
    "  head.append(name,H('span','status '+(R.util[n].part?'bad':'ok'),R.util[n].part?'Registro parcial':'Sin pérdidas'));\n",
    "  if(!lanes.length){sec.appendChild(H('p','muted','Sin intervalos de ejecución registrados.'));return;}\n",
    "\n",
    "  const ctl=sec.appendChild(H('div','ctl'));\n",
    "  const bFirst=H('button','icon-button','⇤'),bPrev=H('button','icon-button','←'),bNext=H('button','icon-button','→'),bLast=H('button','icon-button','⇥');\n",
    "  for(const [button,label] of [[bFirst,'Inicio'],[bPrev,'Ventana anterior'],[bNext,'Ventana siguiente'],[bLast,'Final']]){button.type='button';button.title=label;button.setAttribute('aria-label',label);}\n",
    "  const from=H('input');from.type='number';from.min=0;from.value=0;from.setAttribute('aria-label','Desde tick');\n",
    "  const spanSel=H('select');spanSel.setAttribute('aria-label','Tamaño de la ventana');\n",
    "  for(const s of SPANS){const o=H('option',null,s?s+' ticks':'Todo');o.value=s;spanSel.appendChild(o);}\n",
    "  spanSel.value=end>50?'50':'0';\n",
    "  const slider=H('input');slider.type='range';slider.min=0;slider.value=0;slider.setAttribute('aria-label','Posición de la ventana');\n",
    "  const showEvents=H('input');showEvents.type='checkbox';showEvents.checked=true;\n",
    "  const eventLabel=H('label','event-toggle');eventLabel.append(showEvents,document.createTextNode('Mostrar eventos'));\n",
    "  const pager=H('div','pager');pager.append(bFirst,bPrev,bNext,bLast);\n",
    "  const fromLabel=H('label','control-field','Desde tick'),spanLabel=H('label','control-field','Ventana');fromLabel.appendChild(from);spanLabel.appendChild(spanSel);\n",
    "  ctl.append(pager,fromLabel,spanLabel,slider,eventLabel);\n",
    "\n",
    "  const replay=sec.appendChild(H('div','replay'));\n",
    "  const play=H('button','replay-button','Reproducir'),step=H('button','replay-step','Un tick →');\n",
    "  play.type=step.type='button';play.setAttribute('aria-pressed','false');play.setAttribute('aria-label','Reproducir registro de '+R.nodes[n]);step.setAttribute('aria-label','Avanzar un tick');\n",
    "  const speed=H('select');speed.setAttribute('aria-label','Velocidad de reproducción');\n",
    "  for(const rate of [8,24,60]){const option=H('option',null,rate+' ticks/s');option.value=rate;speed.appendChild(option);}speed.value=8;\n",
    "  const readout=H('output','replay-readout','Reproducción del registro');readout.setAttribute('aria-live','off');\n",
    "  replay.append(play,step,speed,readout);\n",
    "\n",
    "  const lg=sec.appendChild(H('div','legend'));\n",
    "  lg.append(legendItem(\"<svg width='18' height='12'><rect x='1' y='1' width='16' height='10' fill='var(--exec)'/></svg>\",'Ejecución (una celda por tick)'),\n",
    "    legendItem(\"<svg width='12' height='12'><path class='rel' d='M2 11L10 11L6 3Z'/></svg>\",'Liberación'),\n",
    "    legendItem(\"<svg width='12' height='12'><path class='dl' d='M2 2L10 2L6 10Z'/></svg>\",'Plazo absoluto (liberación + D)'),\n",
    "    legendItem(\"<svg width='14' height='14'><path class='ign' d='M7 2L12 7L7 12L2 7Z'/></svg>\",'Pulsación ignorada'),\n",
    "    legendItem(\"<svg width='14' height='14' class='miss'><path d='M3 3L11 11M11 3L3 11'/></svg>\",'Falla de plazo (MISS)'));\n",
    "  const info=sec.appendChild(H('span','chart-info'));\n",
    "  const fig=sec.appendChild(H('div','chart'));fig.tabIndex=0;fig.setAttribute('aria-label','Cuadrícula de ejecución de '+R.nodes[n]+'; desplazamiento horizontal');\n",
    "  let a=0,position=0,playing=false,frameId=0,lastFrame=null,replayVisible=false,paintReplay=null;\n",
    "  const reduced=window.matchMedia('(prefers-reduced-motion: reduce)');\n",
    "  function span(){const s=+spanSel.value;return s>0?Math.min(s,end):end;}\n",
    "  function intervalAt(t){let lo=0,hi=iv.length/3;while(lo<hi){const mid=(lo+hi)>>1;if(iv[mid*3+1]<=t)lo=mid+1;else hi=mid;}const i=(lo-1)*3;return i>=0&&t<iv[i+2]?iv[i]:0;}\n",
    "  function pause(){playing=false;if(frameId)cancelAnimationFrame(frameId);frameId=0;lastFrame=null;play.textContent='Reproducir';play.setAttribute('aria-label','Reproducir registro de '+R.nodes[n]);play.setAttribute('aria-pressed','false');}\n",
    "  function updateReplay(){\n",
    "    if(!replayVisible)return;\n",
    "    const tick=Math.min(Math.floor(position),end),id=intervalAt(tick);\n",
    "    const label='Tick '+tick+' · '+(tick>=end?'Fin del registro':id?'T'+id:'Sin tramo registrado');\n",
    "    if(readout.textContent!==label)readout.textContent=label;\n",
    "    readout.setAttribute('data-tick',tick);readout.setAttribute('data-task',id);\n",
    "    if(paintReplay)paintReplay(position,id);\n",
    "  }\n",
    "  function set(v,follow=false){\n",
    "    if(!follow)pause();a=Math.max(0,Math.min(Math.round(+v)||0,end-span()));\n",
    "    if(!follow)position=a;\n",
    "    from.value=a;slider.max=Math.max(0,end-span());slider.step=Math.max(1,Math.round(span()/20));slider.value=a;fig.scrollLeft=0;draw();\n",
    "  }\n",
    "  function moveTo(t){position=Math.max(0,Math.min(t,end));replayVisible=true;if(position<a)set(position,true);else if(position>=a+span())set(a+span(),true);updateReplay();}\n",
    "  function animate(now){\n",
    "    if(!playing)return;\n",
    "    const elapsed=lastFrame==null?0:Math.min((now-lastFrame)/1000,.1);lastFrame=now;\n",
    "    moveTo(position+elapsed*Number(speed.value));\n",
    "    if(position>=end){pause();return;}\n",
    "    frameId=requestAnimationFrame(animate);\n",
    "  }\n",
    "  play.onclick=function(){\n",
    "    if(playing){pause();return;}if(reduced.matches)return;\n",
    "    if(position>=end){position=0;set(0,true);}\n",
    "    replayVisible=true;playing=true;lastFrame=null;play.textContent='Pausar';play.setAttribute('aria-label','Pausar registro de '+R.nodes[n]);play.setAttribute('aria-pressed','true');updateReplay();frameId=requestAnimationFrame(animate);\n",
    "  };\n",
    "  step.onclick=function(){pause();moveTo(Math.min(Math.floor(position)+1,end));};\n",
    "  function motionPreference(){if(reduced.matches)pause();play.disabled=speed.disabled=reduced.matches;play.title=reduced.matches?'Movimiento reducido: utiliza Un tick para recorrer el registro.':'Reproducir los intervalos registrados';}\n",
    "  motionPreference();reduced.addEventListener('change',motionPreference);\n",
    "  document.addEventListener('visibilitychange',()=>{if(document.hidden)pause();});\n",
    "  bFirst.onclick=()=>set(0);bLast.onclick=()=>set(end);bPrev.onclick=()=>set(a-span());bNext.onclick=()=>set(a+span());\n",
    "  from.onchange=()=>set(from.value);slider.oninput=()=>set(slider.value);spanSel.onchange=()=>set(a);showEvents.onchange=draw;\n",
    "\n",
    "  function draw(){\n",
    "    const sp=span(),b=a+sp,L=88,RM=20,TOP=30,fine=sp<=200,available=Math.max(fig.clientWidth,320);\n",
    "    const cell=Math.min(32,Math.max(20,(available-L-RM)/sp));\n",
    "    const RH=fine?cell:28,W=fine?L+sp*cell+RM:available,H0=TOP+lanes.length*RH,HT=H0+26;\n",
    "    const x=t=>L+(t-a)/sp*(W-L-RM);\n",
    "    fig.textContent='';\n",
    "    const svg=S('svg',{width:W,height:HT,viewBox:'0 0 '+W+' '+HT,role:'img','aria-label':'Gantt del nodo '+R.nodes[n]+', ticks '+a+' a '+b},fig);\n",
    "    S('text',{x:L-12,y:TOP-10,'text-anchor':'end',class:'lane'},svg).textContent='Tick →';\n",
    "    lanes.forEach((id,i)=>{const y=TOP+i*RH,tk=byId.get(id);S('text',{x:L-12,y:y+RH/2+4,class:'lane','text-anchor':'end'},svg).textContent=id?'T'+id+(tk&&tk.ty==='S'?' (S)':''):'Botón';});\n",
    "    for(let i=0;i<iv.length;i+=3){const s=iv[i+1],e=iv[i+2];if(s>=b)break;if(e<=a)continue;const id=iv[i],k=lane.get(id),x0=x(Math.max(s,a)),x1=x(Math.min(e,b));S('rect',{x:x0,y:TOP+k*RH,width:x1-x0,height:RH,class:'execution','data-tip':'T'+id+' ejecuta ['+s+', '+e+') · '+(e-s)+' tick(s)'},svg);}\n",
    "    const labelStep=fine?Math.max(1,Math.ceil(String(b).length*7/cell)):niceStep(sp),gridStep=fine?1:labelStep;\n",
    "    const grid=S('g',{'aria-hidden':'true'},svg);\n",
    "    for(let t=a;t<=b;t+=gridStep){const X=x(t);S('line',{x1:X,x2:X,y1:TOP,y2:H0,class:'grid'+(t%5===0||t===a||t===b?' major':'')},grid);if((t-a)%labelStep===0||t===b)S('text',{x:X,y:TOP-10,'text-anchor':'middle'},svg).textContent=t;}\n",
    "    if((b-a)%gridStep!==0)S('line',{x1:x(b),x2:x(b),y1:TOP,y2:H0,class:'grid major'},grid);\n",
    "    for(let i=0;i<=lanes.length;i++)S('line',{x1:L,x2:W-RM,y1:TOP+i*RH,y2:TOP+i*RH,class:'grid major'},grid);\n",
    "    S('text',{x:L,y:HT-6},svg).textContent=fine?'Cada columna es el intervalo [tick, tick + 1).':'Vista resumida: seleccione una ventana de 200 ticks o menos para ver las celdas.';\n",
    "    const dense=sp>DENSE;let misses=0;\n",
    "    for(let i=0;i<mk.length;i+=4){\n",
    "      const c=mk[i],id=mk[i+1],t=mk[i+2],v=mk[i+3],k=lane.get(id);if(k===undefined)continue;const y=TOP+k*RH,tk=byId.get(id);\n",
    "      if(c===4){if(t<a||t>b)continue;misses++;if(!showEvents.checked)continue;const X=x(t),Y=y+RH/2,d='M'+(X-5)+' '+(Y-5)+'L'+(X+5)+' '+(Y+5)+'M'+(X+5)+' '+(Y-5)+'L'+(X-5)+' '+(Y+5);const g=S('g',{class:'miss','data-tip':'T'+id+' FALLA de plazo en tick '+t},svg);S('path',{d,class:'ring'},g);S('path',{d},g);S('circle',{cx:X,cy:Y,r:9,fill:'transparent'},g);continue;}\n",
    "      if(!showEvents.checked)continue;\n",
    "      if(c===5){if(t<a||t>b)continue;const X=x(t),Y=y+RH/2;S('path',{class:'ign',d:'M'+X+' '+(Y-6)+'L'+(X+6)+' '+Y+'L'+X+' '+(Y+6)+'L'+(X-6)+' '+Y+'Z','data-tip':'Pulsación ignorada en tick '+t+': '+(v===1?'el nodo no tiene tarea esporádica':'no pasó el intervalo mínimo'+(tk?' T='+tk.T:''))},svg);continue;}\n",
    "      if(c!==1||dense)continue;\n",
    "      if(t>=a&&t<=b){const X=x(t);S('path',{class:'rel',d:'M'+(X-4)+' '+(y+RH-1)+'L'+(X+4)+' '+(y+RH-1)+'L'+X+' '+(y+RH-8)+'Z','data-tip':'T'+id+(tk&&tk.ty==='S'?' (esporádica)':'')+' liberada en tick '+t+(tk?' · plazo '+(t+tk.D):'')},svg);}\n",
    "      if(tk){const d=t+tk.D;if(d>=a&&d<=b){const X=x(d);S('path',{class:'dl',d:'M'+(X-4)+' '+(y+2)+'L'+(X+4)+' '+(y+2)+'L'+X+' '+(y+8)+'Z','data-tip':'T'+id+' plazo absoluto en tick '+d+' (liberada en '+t+')'},svg);}}\n",
    "    }\n",
    "    const cursor=S('g',{class:'playhead','aria-hidden':'true'},svg);cursor.style.display='none';\n",
    "    const row=S('rect',{x:L,width:W-L-RM,height:RH,class:'replay-lane'},cursor);\n",
    "    const selected=S('rect',{height:RH-2,class:'replay-cell'},cursor);\n",
    "    const line=S('line',{y1:TOP-7,y2:H0+5},cursor),cap=S('circle',{cy:TOP-8,r:3},cursor);\n",
    "    paintReplay=function(t,id){\n",
    "      cursor.style.display=replayVisible&&t>=a&&t<=b?'':'none';\n",
    "      if(t<a||t>b)return;const X=x(t),k=lane.get(id);\n",
    "      line.setAttribute('x1',X);line.setAttribute('x2',X);cap.setAttribute('cx',X);\n",
    "      const active=id&&k!==undefined&&t<end;row.style.display=selected.style.display=active?'':'none';\n",
    "      if(active){row.setAttribute('y',TOP+k*RH);selected.setAttribute('x',x(Math.floor(t))+1);selected.setAttribute('y',TOP+k*RH+1);selected.setAttribute('width',Math.max(1,(W-L-RM)/sp-2));}\n",
    "      if(playing&&(X<fig.scrollLeft+L||X>fig.scrollLeft+fig.clientWidth-20))fig.scrollLeft=Math.max(0,X-fig.clientWidth*.55);\n",
    "    };\n",
    "    info.textContent='Ticks '+a+'–'+b+' de '+end+' · '+(fine?'1 celda = 1 tick':'vista resumida')+' · fallas en la ventana: '+misses+(dense?' · acerque la ventana a ≤'+DENSE+' ticks para ver liberaciones y plazos':'');\n",
    "    updateReplay();\n",
    "  }\n",
    "  window.addEventListener('resize',draw);set(0);\n",
    "}\n",
    "\n",
    "const g=document.getElementById('gantt');\n",
    "for(let n=0;n<R.nodes.length;n++)nodeChart(n,g);\n",
    "\n",
    "document.getElementById('util').append(table(\n",
    "  [['Nodo'],['U teórica'],['U medida'],['Cambios de tarea'],['Ticks observados'],['Telemetría','l']],\n",
    "  R.util.map(function(u,n){const c=u.cnt;\n",
    "    return [R.nodes[n],u.th.toFixed(4),u.end?u.obs.toFixed(4)+(u.part?' (parcial)':''):'sin datos',u.sw,u.end,\n",
    "      H('span',u.part?'bad':'ok',(u.part?'INCOMPLETA':'sin pérdidas')+' · misses='+c[0]+', esporádicas ignoradas='+c[1]+', ticks perdidos='+c[2]+', eventos descartados='+c[3]+', tramas malas='+c[4]+', descartes de cola='+u.drop)];})),\n",
    "  H('p','muted','U teórica = Σ C/T de las tareas confirmadas en el nodo. U medida = ticks de ejecución reconstruidos / ticks observados desde START; con pérdidas es parcial. Cambios de tarea = entradas de una tarea distinta en ejecución (SWITCH). Descartes del coordinador: '+R.drop+'.'));\n",
    "\n",
    "document.getElementById('stats').append(table(\n",
    "  [['Nodo'],['Tarea'],['Tipo'],['C'],['T'],['D'],['C/T'],['Liberaciones'],['Ticks ejecutados'],['Completadas'],['Fallas de plazo'],['Esporádicas ignoradas'],['Latencia 1ª ejecución'],['Respuesta media'],['Respuesta máx.']],\n",
    "  R.tasks.slice().sort(function(p,q){return p.n-q.n||p.id-q.id;}).map(function(t){\n",
    "    return [R.nodes[t.n],'T'+t.id,t.ty==='S'?'Esporádica':'Periódica',t.C,t.T,t.D,t.T?(t.C/t.T).toFixed(3):'—',t.rel,t.run,t.comp,\n",
    "      t.miss?H('span','bad','✕ '+t.miss):'0',t.ign,fixed(t.f,2),fixed(t.c,2),t.cm==null?'—':t.cm];})),\n",
    "  H('p','muted','Tiempos en ticks del nodo. Latencia 1ª ejecución = inicio de la primera ejecución − liberación; tiempo de respuesta = finalización − liberación (media y máximo sobre trabajos correlacionables).'));\n",
    "\n",
    "const pr=[];\n",
    "for(let n=0;n<R.nodes.length;n++){const mk=R.mk[n],s=R.tasks.find(function(t){return t.n===n&&t.ty==='S';});let rel=0,minI=0,noT=0;\n",
    "  for(let i=0;i<mk.length;i+=4){if(mk[i]===1&&s&&mk[i+1]===s.id)rel++;if(mk[i]===5){if(mk[i+3]===1)noT++;else minI++;}}\n",
    "  const lost=R.util[n].cnt[1]-(minI+noT);\n",
    "  pr.push([R.nodes[n],s?'T'+s.id+' (T mín.='+s.T+', D='+s.D+')':'—',rel,s?fixed(s.f,2):'—',s?fixed(s.c,2):'—',s&&s.cm!=null?s.cm:'—',s&&s.miss?H('span','bad','✕ '+s.miss):'0',minI,noT?H('span','bad','✕ '+noT):'0',R.util[n].cnt[1],lost>0?H('span','bad',lost+' sin evento'):'0']);}\n",
    "document.getElementById('presses').append(table(\n",
    "  [['Nodo'],['Tarea esporádica'],['Liberaciones'],['Latencia 1ª ejecución'],['Respuesta media'],['Respuesta máx.'],['Fallas de plazo'],['Ignoradas: intervalo mínimo'],['Ignoradas: nodo sin esporádica'],['Ignoradas según contador del nodo'],['Sin evento']],pr),\n",
    "  H('p','muted','Cada nodo admite como máximo una esporádica, liberada por el botón de ese nodo (o por trigger desde el host). Una pulsación antes de T ticks desde la última liberación aceptada se ignora y se cuenta; si el nodo no tiene esporádica asignada, la pulsación también se ignora. Latencias en ticks del nodo: 1ª ejecución = inicio − liberación; respuesta = finalización − liberación. «Sin evento» = ignoradas que el nodo contó pero cuyo evento no llegó (más de 8 por tick o cola llena).'));\n",
    "\n",
    "const t0=R.dec.length?R.dec[0].ns:0;\n",
    "const adm=document.getElementById('admit');\n",
    "adm.append(H('p',null,nAcc+' aceptada(s) · '+nRej+' rechazada(s) · '+(finals.length-nAcc-nRej)+' en otro estado · '+R.dec.filter(d=>d.k===5).length+' intento(s) de alta rechazado(s). Estado final de cada tarea:'),\n",
    "  table([['Tarea'],['Tipo'],['W'],['T'],['D'],['Criticidad'],['Nodo'],['Estado','l'],['Motivo','why']],\n",
    "    finals.map(function(d){return ['T'+d.id,d.ty==='S'?'Esporádica':'Periódica',d.W,d.T,d.D,d.cr,d.n<0?'—':R.nodes[d.n],badge(d.k),d.why];})));\n",
    "const det=adm.appendChild(H('details'));det.appendChild(H('summary',null,'Historial completo de decisiones ('+R.dec.length+')'));\n",
    "det.appendChild(table([['t (s)'],['Tarea'],['Nodo'],['Estado','l'],['Motivo','why']],\n",
    "  R.dec.map(function(d){return [((d.ns-t0)/1e9).toFixed(3),'T'+d.id,d.n<0?'—':R.nodes[d.n],badge(d.k),d.why];})));\n",
    "})();\n",
};

static void put_lines(FILE *f,const char *const *lines,size_t count) {
    for(size_t i=0;i<count;i++)fputs(lines[i],f);
}

// Une datos y plantilla; U medida es ticks ocupados reconstruidos / ticks observados.
static void write_html(FILE *html,FILE *gantt,FILE *marks,FILE *decisions,const Stat *stats,size_t n,
                       const long double theoretical[NODES],const uint64_t busy[NODES],const uint64_t end_tick[NODES],
                       const uint64_t switches[NODES],uint32_t counters[NODES][5],const bool incomplete[NODES],
                       const uint64_t dropped[3]) {
    put_lines(html,PAGE_HEAD,sizeof PAGE_HEAD/sizeof *PAGE_HEAD);
    fprintf(html,"<script>\nconst R={nodes:['ESP32','Pico'],drop:%"PRIu64",\nutil:[",dropped[2]);
    for(int i=0;i<NODES;i++) {
        fprintf(html,"{th:%.6Lf,obs:%.6Lf,part:%d,sw:%"PRIu64",end:%"PRIu64",drop:%"PRIu64",cnt:[%u,%u,%u,%u,%u]},",
                // La carga teórica es la final; la medida resume el horizonte observado, incluso si hubo migración.
                theoretical[i],end_tick[i]?(long double)busy[i]/end_tick[i]:0.0L,incomplete[i],switches[i],end_tick[i],dropped[i],
                counters[i][0],counters[i][1],counters[i][2],counters[i][3],counters[i][4]);
    }
    fputs("],\ntasks:[\n",html);
    for(size_t i=0;i<n;i++) {
        const Stat *p=&stats[i];
        fprintf(html,"{n:%d,id:%"PRIu32",ty:'%c',W:%"PRIu32",T:%"PRIu32",D:%"PRIu32",C:%"PRIu32",rel:%"PRIu64",run:%"PRIu64",comp:%"PRIu64",miss:%"PRIu64",ign:%"PRIu64",f:",
                p->node,p->id,p->task.type=='S'?'S':'P',p->task.W,p->task.T,p->task.D,p->cost,p->releases,p->runs,p->completed,p->misses,p->ignored);
        // Sin muestras usa null para distinguir ausencia de datos de una latencia medida de cero.
        if(p->first_n)fprintf(html,"%.3Lf",(long double)p->first_sum/p->first_n);
        else fputs("null",html);
        fputs(",c:",html);
        if(p->finish_n)fprintf(html,"%.3Lf,cm:%"PRIu64,(long double)p->finish_sum/p->finish_n,p->finish_max);
        else fputs("null,cm:null",html);
        fputs("},\n",html);
    }

    char line[128];
    fputs("],\niv:[",html);
    for(int i=0;i<NODES;i++) {
        rewind(gantt);
        if(!fgets(line,sizeof line,gantt))line[0]=0;
        fputc('[',html);
        int node;
        uint32_t id;
        uint64_t begin,end;
        while(fgets(line,sizeof line,gantt))
            if(sscanf(line,"%d,%"SCNu32",%"SCNu64",%"SCNu64,&node,&id,&begin,&end)==4 && node==i)
                fprintf(html,"%"PRIu32",%"PRIu64",%"PRIu64",",id,begin,end);
        fputs("],\n",html);
    }
    fputs("],\nmk:[",html);
    for(int i=0;i<NODES;i++) {
        rewind(marks);
        fputc('[',html);
        int node,code;
        uint32_t id,tick,value;
        while(fscanf(marks,"%d %d %"SCNu32" %"SCNu32" %"SCNu32,&node,&code,&id,&tick,&value)==5)
            if(node==i)fprintf(html,"%d,%"PRIu32",%"PRIu32",%"PRIu32",",code,id,tick,value);
        fputs("],\n",html);
    }
    fputs("],\ndec:[\n",html);
    rewind(decisions);
    size_t got;
    char buffer[4096];
    while((got=fread(buffer,1,sizeof buffer,decisions))>0)fwrite(buffer,1,got,html);
    fputs("]};\n",html);
    put_lines(html,PAGE_SCRIPT,sizeof PAGE_SCRIPT/sizeof *PAGE_SCRIPT);
    fputs("</script>\n</body>\n</html>\n",html);
}

// El reporte solo consume IPC; los drivers son quienes leen los puertos seriales.
int report_run(Shared *s,const Config *c) {
    if(mkdir(c->output,0755)<0 && errno!=EEXIST)return 1;
    FILE *events=open_output(c,"events.csv"),*gantt=open_output(c,"gantt.csv"),*statsfile=open_output(c,"tasks.csv"),*html=open_output(c,"report.html");
    if(!events || !gantt || !statsfile || !html) {
        if(events)fclose(events);
        if(gantt)fclose(gantt);
        if(statsfile)fclose(statsfile);
        if(html)fclose(html);
        return 1;
    }
    Stat *stats=calloc(TASK_CAP*NODES,sizeof *stats);

    FILE *marks=tmpfile(),*decisions=tmpfile();
    if(!stats || !marks || !decisions) {
        free(stats);
        if(marks)fclose(marks);
        if(decisions)fclose(decisions);
        fclose(events);
        fclose(gantt);
        fclose(statsfile);
        fclose(html);
        return 1;
    }
    size_t n=0;
    uint64_t busy[NODES]={0},end_tick[NODES]={0},switches[NODES]={0};
    Trace traces[NODES]={{.valid=true},{.valid=true}};
    uint32_t counters[NODES][5]={{0}};
    bool incomplete[NODES]={false};
    long double theoretical[2]= {
        0
    };
    fprintf(events,"host_monotonic_ns,node,kind,code,tick,id,value,detail\n");
    fprintf(gantt,"node,id,start_tick,end_tick\n");
    pid_t parent=getppid();
    bool done=parent==1;
    while(!done) {
        Record r;

        // Espera con semáforos y revisa si murió el coordinador cuando no recibe mensajes.
        if(queue_pop(&s->events,&r,500)<0) {
            if(getppid()!=parent)break;
            continue;
        }
        if(r.kind==R_END) {
            done=true;
            continue;
        }
        fprintf(events,"%"PRIu64",%d,%d,%d,%"PRIu32",%"PRIu32",%"PRIu32",",r.ns,r.node,r.kind,r.code,r.tick,r.id,r.value);
        csv_text(events,r.text);
        fputc('\n',events);

        // Guarda decisiones sin destino antes de filtrar los mensajes generales con node=-1.
        if(r.kind==R_TASK) {
            fprintf(decisions,"{ns:%"PRIu64",n:%d,id:%"PRIu32",k:%d,ty:'%c',W:%"PRIu32",T:%"PRIu32",D:%"PRIu32",cr:%"PRId32",C:%"PRIu32",why:",
                    r.ns,r.node,r.task.id,r.code,r.task.type=='S'?'S':'P',r.task.W,r.task.T,r.task.D,r.task.criticality,r.value);
            js_text(decisions,r.text);
            fputs("},\n",decisions);
        }
        if(r.node<0 || r.node>=NODES)continue;
        int node=r.node;
        Trace *trace=&traces[node];

        // El heartbeat amplía el horizonte, pero no adelanta el Gantt sobre eventos aún pendientes.
        if(r.kind==R_TICK) {

            uint64_t horizon=(uint64_t)r.tick+1;
            if(horizon>end_tick[node])end_tick[node]=horizon;
            for(size_t i=0;i<5;i++) {
                const uint8_t *p=r.frame.data+4+2*i;
                uint32_t value=(uint32_t)p[0]|((uint32_t)p[1]<<8);
                if(value>counters[node][i]) {
                    if(i>=2)incomplete[node]=true;
                    if(i==2 || i==3)trace->valid=false;
                    counters[node][i]=value;
                }
            }
            continue;
        }
        // STOP confirmado permite cerrar solo hasta el último límite de ejecución observado.
        if(r.kind==R_STOP) {

            advance_trace(trace,end_tick[node],node,stats,&n,gantt,&busy[node],&incomplete[node]);
            trace->selected=0;
            continue;
        }
        if(r.kind==R_NOTE) {
            if(strstr(r.text,"corrupt") || strstr(r.text,"descartad") || strstr(r.text,"timeout") || strstr(r.text,"desconexión") || strstr(r.text,"inválida") || strstr(r.text,"inesperada") || strstr(r.text,"FALLA")) {
                incomplete[node]=true;
                trace->valid=false;
            }
            continue;
        }
        // La carga teórica cambia solo con despliegues confirmados y retiros.
        if(r.kind==R_TASK) {
            Stat *st=find_stat(stats,&n,r.task.id,node);
            if(!st) {
                incomplete[node]=true;
                continue;
            }
            st->task=r.task;
            st->cost=r.value;

            if(r.code==1 && !st->confirmed) {
                theoretical[node]+=(long double)r.value/r.task.T;
                st->confirmed=true;
            }
            if(r.code==2 && st->confirmed) {
                theoretical[node]-=(long double)r.value/r.task.T;
                st->confirmed=false;
            }
            continue;
        }
        if(r.kind!=R_NODE)continue;
        if(r.tick>end_tick[node])end_tick[node]=r.tick;

        // Salta el hueco de ticks perdidos y espera una selección conocida para retomar la traza.
        if(r.code==RT_TICK_LOST) {
            uint64_t gap_start=r.tick>=r.value?r.tick-r.value:0;
            advance_trace(trace,gap_start,node,stats,&n,gantt,&busy[node],&incomplete[node]);
            if(r.tick>trace->cursor)trace->cursor=r.tick;
            trace->valid=false;
            incomplete[node]=true;
            continue;
        }
        if(r.tick<trace->cursor) {
            incomplete[node]=true;
            trace->valid=false;
            continue;
        }
        advance_trace(trace,r.tick,node,stats,&n,gantt,&busy[node],&incomplete[node]);
        // Un SWITCH recupera una selección conocida para reconstruir ejecución después de un hueco.
        if(r.code==RT_SWITCH) {
            if(r.id && r.id!=trace->selected)switches[node]++;
            trace->selected=r.id;
            trace->valid=true;
            continue;
        }
        if(r.code==RT_BAD_FRAME) {
            incomplete[node]=true;
            continue;
        }

        if(r.code==RT_RELEASE || r.code==RT_MISS || r.code==RT_SPORADIC_IGNORED)
            fprintf(marks,"%d %d %"PRIu32" %"PRIu32" %"PRIu32"\n",node,r.code,r.id,r.tick,r.value);
        if(!r.id || r.code==RT_TASK_ADDED)continue;
        Stat *st=find_stat(stats,&n,r.id,node);
        if(!st) {
            incomplete[node]=true;
            continue;
        }

        // Empareja RELEASE con COMPLETE/MISS por número de trabajo para medir respuestas válidas.
        if(r.code==RT_RELEASE) {
            st->releases++;
            if(st->pending==RT_MAX_JOBS) {
                incomplete[node]=true;
                trace->valid=false;
                st->pending=0;
            }
            st->jobs[st->pending++]=(JobStat){.release=r.tick,.sequence=r.value};
        } else if(r.code==RT_COMPLETE || r.code==RT_MISS) {
            if(r.code==RT_COMPLETE)st->completed++;
            else st->misses++;
            for(size_t j=0;j<st->pending;j++)if(st->jobs[j].sequence==r.value) {
                if(r.code==RT_COMPLETE && r.tick>=st->jobs[j].release) {
                    uint64_t response=r.tick-st->jobs[j].release;
                    st->finish_sum+=response;
                    st->finish_n++;
                    if(response>st->finish_max)st->finish_max=response;
                }
                memmove(st->jobs+j,st->jobs+j+1,(st->pending-j-1)*sizeof *st->jobs);
                st->pending--;
                break;
            }
        } else if(r.code==RT_SPORADIC_IGNORED)st->ignored++;
    }
    uint64_t dropped[3]= {
        0
    };

    // Copia descartes bajo lock; si no logra leerlos, marca ambas trazas incompletas.
    if(sem_wait_ms(&s->lock,20)==0) {
        memcpy(dropped,s->dropped,sizeof dropped);
        sem_post(&s->lock);
    } else {
        incomplete[0]=true;
        incomplete[1]=true;
    }
    for(int i=0;i<2;i++)if(dropped[i] || dropped[2])incomplete[i]=true;
    fprintf(statsfile,"node,id,type,W,T,D,criticality,C,releases,run_ticks,completed,deadline_misses,sporadic_ignored,first_samples,mean_first_ticks,completion_samples,mean_completion_ticks,max_completion_ticks\n");
    for(size_t i=0;i<n;i++) {
        Stat *p=&stats[i];
        fprintf(statsfile,"%d,%"PRIu32",%c,%"PRIu32",%"PRIu32",%"PRIu32",%"PRId32",%"PRIu32",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",",p->node,p->id,p->task.type?p->task.type:'?',p->task.W,p->task.T,p->task.D,p->task.criticality,p->cost,p->releases,p->runs,p->completed,p->misses,p->ignored,p->first_n);
        if(p->first_n)fprintf(statsfile,"%.3Lf",(long double)p->first_sum/p->first_n);
        fprintf(statsfile,",%"PRIu64",",p->finish_n);
        if(p->finish_n)fprintf(statsfile,"%.3Lf,%"PRIu64,(long double)p->finish_sum/p->finish_n,p->finish_max);
        else fputc(',',statsfile);
        fputc('\n',statsfile);
    }

    // Con todos los datos consumidos, genera el HTML y cierra archivos comprobando errores de salida.
    write_html(html,gantt,marks,decisions,stats,n,theoretical,busy,end_tick,switches,counters,incomplete,dropped);
    fclose(marks);
    fclose(decisions);
    bool bad=ferror(events)||ferror(gantt)||ferror(statsfile)||ferror(html);
    if(fclose(events)!=0)bad=true;
    if(fclose(gantt)!=0)bad=true;
    if(fclose(statsfile)!=0)bad=true;
    if(fclose(html)!=0)bad=true;
    free(stats);
    return bad?1:0;
}
