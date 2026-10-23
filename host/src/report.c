#include "host.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct {
    uint32_t id;
    int node;
    Task task;
    uint64_t releases,runs,completed,misses,ignored;
    uint64_t first_sum,finish_sum,first_n,finish_n,finish_max,release;
    bool active,first,ambiguous,confirmed;
    uint32_t cost,job;
} Stat;
static FILE *open_output(const Config *c,const char *name) {
    char path[1100];
    snprintf(path,sizeof path,"%s/%s",c->output,name);
    return fopen(path,"w+");
}

static void csv_text(FILE *f,const char *s) {
    fputc('"',f);
    for(;*s;s++) {
        if(*s=='"')fputc('"',f);
        fputc(*s,f);
    }
    fputc('"',f);
}

/* Cadena JS segura dentro de <script>: escapa comillas, \\ y '<'. */
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

/* SWITCH mantiene la selección hasta el siguiente cambio. COMPLETE/MISS
 * cierran el trabajo aunque el siguiente SWITCH todavía no haya llegado. */
typedef struct {
    uint64_t cursor;
    uint32_t selected;
    bool valid;
} Trace;

static void advance_trace(Trace *trace,uint64_t until,int node,Stat *stats,size_t *n,
                          FILE *gantt,uint64_t *busy,bool *incomplete) {
    if(until<=trace->cursor)return;
    if(trace->valid && trace->selected) {
        Stat *st=find_stat(stats,n,trace->selected,node);
        if(st && st->active) {
            fprintf(gantt,"%d,%"PRIu32",%"PRIu64",%"PRIu64"\n",
                    node,st->id,trace->cursor,until);
            uint64_t duration=until-trace->cursor;
            *busy+=duration;
            st->runs+=duration;
            if(!st->first && !st->ambiguous && trace->cursor>=st->release) {
                st->first_sum+=trace->cursor-st->release;
                st->first_n++;
                st->first=true;
            }
        } else if(!st)*incomplete=true;
    }
    trace->cursor=until;
}

static const char *const PAGE_HEAD[]= {
    "<!doctype html>\n",
    "<html lang='es'>\n",
    "<head>\n",
    "<meta charset='utf-8'>\n",
    "<meta name='viewport' content='width=device-width,initial-scale=1'>\n",
    "<title>Reporte del calendarizador</title>\n",
    "<style>\n",
    ":root{color-scheme:light;--page:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--base:#c3c2b7;--tick-grid:#aeb5ad;--tick-major:#778476;--exec:#00bd55;--crit:#d03b3b;--good:#006300;\n",
    "--s1:#2a78d6;--s2:#eb6834;--s3:#1baf7a;--s4:#eda100;--s5:#e87ba4;--s6:#008300;--s7:#6250d6;--s8:#e34948}\n",
    "@media (prefers-color-scheme:dark){:root:not([data-theme='light']){color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--grid:#2c2c2a;--base:#383835;--tick-grid:#465246;--tick-major:#718371;--exec:#14c96a;--good:#0ca30c;\n",
    "--s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500;--s5:#d55181;--s6:#008300;--s7:#9085e9;--s8:#e66767}}\n",
    ":root[data-theme='dark']{color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--grid:#2c2c2a;--base:#383835;--tick-grid:#465246;--tick-major:#718371;--exec:#14c96a;--good:#0ca30c;\n",
    "--s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500;--s5:#d55181;--s6:#008300;--s7:#9085e9;--s8:#e66767}\n",
    "*{box-sizing:border-box}\n",
    "body{margin:0;background:var(--page);color:var(--ink);font:15px/1.5 system-ui,sans-serif}\n",
    "main{max-width:1200px;margin:0 auto;padding:24px 16px 48px}\n",
    "h1{font-size:1.6em;margin:0 0 .3em}h2{margin:2em 0 .5em;font-size:1.25em}h3{margin:0 0 .5em;font-size:1.05em}\n",
    "p{color:var(--ink2);max-width:75ch}.muted{color:var(--muted)}\n",
    "a{color:var(--s1)}\n",
    ".card{background:var(--surface);border:1px solid var(--grid);border-radius:8px;padding:16px;margin:12px 0}\n",
    ".ctl{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:8px}\n",
    ".ctl input[type=number]{width:7em}.ctl input[type=range]{flex:1;min-width:120px}\n",
    "button,select,input{font:inherit;color:var(--ink);background:var(--page);border:1px solid var(--base);border-radius:6px;padding:2px 8px}\n",
    "button{cursor:pointer}\n",
    ".legend{display:flex;flex-wrap:wrap;gap:16px;font-size:.9em;color:var(--ink2);margin-bottom:6px}\n",
    ".legend svg{vertical-align:-2px;margin-right:4px}\n",
    ".chart{width:100%;overflow-x:auto;padding-bottom:4px}.chart>svg{display:block}\n",
    ".chart text{font-size:12px;fill:var(--muted);font-variant-numeric:tabular-nums}\n",
    ".chart text.lane{fill:var(--ink2);font-weight:600}\n",
    ".grid{stroke:var(--tick-grid);stroke-width:1;shape-rendering:crispEdges;pointer-events:none}.grid.major{stroke:var(--tick-major)}.execution{fill:var(--exec)}\n",
    ".rel{fill:var(--ink)}.dl{fill:none;stroke:var(--ink2);stroke-width:1.5}\n",
    ".ign{fill:var(--surface);stroke:var(--ink2);stroke-width:1.5}\n",
    ".miss path{stroke:var(--crit);stroke-width:2.5;stroke-linecap:round}.miss path.ring{stroke:var(--surface);stroke-width:6}\n",
    ".scroll{overflow-x:auto}\n",
    "table{border-collapse:collapse;font-variant-numeric:tabular-nums;width:100%}\n",
    "th,td{padding:6px 10px;border-bottom:1px solid var(--grid);text-align:right;white-space:nowrap}\n",
    "th{color:var(--ink2);font-weight:600;border-bottom-color:var(--base)}\n",
    "th:first-child,td:first-child,td.l,th.l{text-align:left}td.why{text-align:left;white-space:normal;min-width:20em}\n",
    ".bad{color:var(--crit);font-weight:600}.ok{color:var(--good);font-weight:600}\n",
    "#tip{position:fixed;pointer-events:none;background:var(--surface);color:var(--ink);border:1px solid var(--base);border-radius:6px;padding:4px 8px;font-size:13px;box-shadow:0 2px 8px #0003;z-index:9}\n",
    "details summary{cursor:pointer;color:var(--ink2)}\n",
    "</style>\n",
    "</head>\n",
    "<body>\n",
    "<main>\n",
    "<h1>Calendarizador heterogéneo</h1>\n",
    "<p>Gantt reconstruido a partir de los eventos SWITCH, RELEASE, COMPLETE y MISS recibidos. En la vista de detalle, cada celda representa un tick y cada fila una tarea; las celdas verdes indican ejecución. Cada nodo usa su propio tick local. Los tramos sin bloques no prueban que el nodo estuviera inactivo, y un intervalo con pérdidas cuenta como incompleto.</p>\n",
    "<p><a href='events.csv'>Eventos y decisiones (CSV)</a> · <a href='gantt.csv'>Intervalos (CSV)</a> · <a href='tasks.csv'>Estadísticas por tarea (CSV)</a></p>\n",
    "<h2>Diagrama de Gantt por nodo</h2>\n",
    "<div id='gantt'></div>\n",
    "<h2>Utilización y cambios de tarea por nodo</h2>\n",
    "<div class='card scroll' id='util'></div>\n",
    "<h2>Estadísticas por tarea</h2>\n",
    "<div class='card scroll' id='stats'></div>\n",
    "<h2>Esporádicas y pulsaciones del botón</h2>\n",
    "<div class='card scroll' id='presses'></div>\n",
    "<h2>Tareas aceptadas y rechazadas</h2>\n",
    "<div class='card scroll' id='admit'></div>\n",
    "<div id='tip' hidden></div>\n",
    "</main>\n",
};

static const char *const PAGE_SCRIPT[]= {
    "(function(){\n",
    "'use strict';\n",
    "const STATE={0:['…','Admitida (pendiente)',''],1:['✓','Aceptada','ok'],2:['↺','Retirada','bad'],3:['✕','Rechazada','bad'],4:['?','Incierta','bad']};\n",
    "const SPANS=[20,50,100,200,500,1000,5000,0];\n",
    "const DENSE=1500;\n",
    "const NS='http://www.w3.org/2000/svg';\n",
    "function S(t,a,p){const e=document.createElementNS(NS,t);for(const k in a)e.setAttribute(k,a[k]);if(p)p.appendChild(e);return e;}\n",
    "function H(t,c,txt){const e=document.createElement(t);if(c)e.className=c;if(txt!=null)e.textContent=txt;return e;}\n",
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
    "  return t;\n",
    "}\n",
    "function badge(code){const s=STATE[code]||['?','Código '+code,''];return H('span',s[2],s[0]+' '+s[1]);}\n",
    "function legendItem(svg,label){const s=H('span');s.innerHTML=svg;s.appendChild(document.createTextNode(label));return s;}\n",
    "\n",
    "function nodeChart(n,root){\n",
    "  const tasks=R.tasks.filter(function(t){return t.n===n;});\n",
    "  const byId=new Map(tasks.map(function(t){return [t.id,t];}));\n",
    "  const iv=R.iv[n],mk=R.mk[n],ids=new Set(tasks.map(function(t){return t.id;}));\n",
    "  for(let i=0;i<iv.length;i+=3)ids.add(iv[i]);\n",
    "  const lanes=[...ids].sort(function(a,b){return a-b;});\n",
    "  /* Fila extra para pulsaciones en un nodo sin tarea esporádica (id 0). */\n",
    "  for(let i=0;i<mk.length;i+=4)if(mk[i]===5&&mk[i+1]===0){lanes.push(0);break;}\n",
    "  const lane=new Map(lanes.map(function(id,i){return [id,i];}));\n",
    "  const end=Math.max(R.util[n].end,1);\n",
    "  const sec=root.appendChild(H('section','card'));\n",
    "  sec.appendChild(H('h3',null,'Nodo '+R.nodes[n]));\n",
    "  if(!lanes.length){sec.appendChild(H('p','muted','Sin intervalos de ejecución registrados.'));return;}\n",
    "  const ctl=sec.appendChild(H('div','ctl'));\n",
    "  const bFirst=H('button',null,'⏮'),bPrev=H('button',null,'◀'),bNext=H('button',null,'▶'),bLast=H('button',null,'⏭');\n",
    "  bFirst.title='Inicio';bPrev.title='Ventana anterior';bNext.title='Ventana siguiente';bLast.title='Final';\n",
    "  const from=H('input');from.type='number';from.min=0;from.value=0;\n",
    "  const spanSel=H('select');\n",
    "  for(const s of SPANS){const o=H('option',null,s?s+' ticks':'Todo');o.value=s;spanSel.appendChild(o);}\n",
    "  spanSel.value=end>50?'50':'0';\n",
    "  const slider=H('input');slider.type='range';slider.min=0;slider.value=0;slider.setAttribute('aria-label','Posición de la ventana');\n",
    "  const showEvents=H('input');showEvents.type='checkbox';showEvents.checked=true;\n",
    "  const eventLabel=H('label');eventLabel.append(showEvents,document.createTextNode(' Mostrar eventos'));\n",
    "  const info=H('span','muted');\n",
    "  ctl.append(bFirst,bPrev,H('label',null,'Desde tick'),from,H('label',null,'Ventana'),spanSel,bNext,bLast,slider,eventLabel);\n",
    "  const lg=sec.appendChild(H('div','legend'));\n",
    "  lg.append(\n",
    "    legendItem(\"<svg width='18' height='12'><rect x='1' y='1' width='16' height='10' fill='var(--exec)'/></svg>\",'Ejecución (una celda por tick)'),\n",
    "    legendItem(\"<svg width='12' height='12'><path class='rel' d='M2 11L10 11L6 3Z'/></svg>\",'Liberación'),\n",
    "    legendItem(\"<svg width='12' height='12'><path class='dl' d='M2 2L10 2L6 10Z'/></svg>\",'Plazo absoluto (liberación + D)'),\n",
    "    legendItem(\"<svg width='14' height='14'><path class='ign' d='M7 2L12 7L7 12L2 7Z'/></svg>\",'Pulsación ignorada'),\n",
    "    legendItem(\"<svg width='14' height='14' class='miss'><path d='M3 3L11 11M11 3L3 11'/></svg>\",'Falla de plazo (MISS)'));\n",
    "  sec.appendChild(info);\n",
    "  const fig=sec.appendChild(H('div','chart'));fig.tabIndex=0;fig.setAttribute('aria-label','Cuadrícula de ejecución de '+R.nodes[n]+'; desplazamiento horizontal');\n",
    "  let a=0;\n",
    "  function span(){const s=+spanSel.value;return s>0?Math.min(s,end):end;}\n",
    "  function set(v){a=Math.max(0,Math.min(Math.round(+v)||0,end-span()));from.value=a;slider.max=Math.max(0,end-span());slider.step=Math.max(1,Math.round(span()/20));slider.value=a;draw();}\n",
    "  bFirst.onclick=function(){set(0);};bLast.onclick=function(){set(end);};\n",
    "  bPrev.onclick=function(){set(a-span());};bNext.onclick=function(){set(a+span());};\n",
    "  from.onchange=function(){set(from.value);};slider.oninput=function(){set(slider.value);};spanSel.onchange=function(){set(a);};showEvents.onchange=draw;\n",
    "  function draw(){\n",
    "    const sp=span(),b=a+sp,L=88,RM=20,TOP=30,fine=sp<=200;\n",
    "    const available=Math.max(fig.clientWidth,320);\n",
    "    const cell=Math.min(32,Math.max(20,(available-L-RM)/sp));\n",
    "    const RH=fine?cell:28,W=fine?L+sp*cell+RM:available,H0=TOP+lanes.length*RH,HT=H0+26;\n",
    "    const x=function(t){return L+(t-a)/sp*(W-L-RM);};\n",
    "    fig.textContent='';\n",
    "    const svg=S('svg',{width:W,height:HT,viewBox:'0 0 '+W+' '+HT,role:'img','aria-label':'Gantt del nodo '+R.nodes[n]+', ticks '+a+' a '+b},fig);\n",
    "    S('text',{x:L-12,y:TOP-10,'text-anchor':'end',class:'lane'},svg).textContent='Tick →';\n",
    "    lanes.forEach(function(id,i){const y=TOP+i*RH,tk=byId.get(id);\n",
    "      S('text',{x:L-12,y:y+RH/2+4,class:'lane','text-anchor':'end'},svg).textContent=id?'T'+id+(tk&&tk.ty==='S'?' (S)':''):'Botón';});\n",
    "    for(let i=0;i<iv.length;i+=3){const s=iv[i+1],e=iv[i+2];if(s>=b)break;if(e<=a)continue;\n",
    "      const id=iv[i],k=lane.get(id),x0=x(Math.max(s,a)),x1=x(Math.min(e,b));\n",
    "      S('rect',{x:x0,y:TOP+k*RH,width:x1-x0,height:RH,class:'execution','data-tip':'T'+id+' ejecuta ['+s+', '+e+') · '+(e-s)+' tick(s)'},svg);}\n",
    "    const labelStep=fine?Math.max(1,Math.ceil(String(b).length*7/cell)):niceStep(sp);\n",
    "    const gridStep=fine?1:labelStep;\n",
    "    const grid=S('g',{'aria-hidden':'true'},svg);\n",
    "    for(let t=a;t<=b;t+=gridStep){const X=x(t);\n",
    "      S('line',{x1:X,x2:X,y1:TOP,y2:H0,class:'grid'+(t%5===0||t===a||t===b?' major':'')},grid);\n",
    "      if((t-a)%labelStep===0||t===b)S('text',{x:X,y:TOP-10,'text-anchor':'middle'},svg).textContent=t;}\n",
    "    if((b-a)%gridStep!==0)S('line',{x1:x(b),x2:x(b),y1:TOP,y2:H0,class:'grid major'},grid);\n",
    "    for(let i=0;i<=lanes.length;i++)S('line',{x1:L,x2:W-RM,y1:TOP+i*RH,y2:TOP+i*RH,class:'grid major'},grid);\n",
    "    S('text',{x:L,y:HT-6},svg).textContent=fine?'Cada columna es el intervalo [tick, tick + 1).':'Vista resumida: seleccione una ventana de 200 ticks o menos para ver las celdas.';\n",
    "    const dense=sp>DENSE;let misses=0;\n",
    "    for(let i=0;i<mk.length;i+=4){const c=mk[i],id=mk[i+1],t=mk[i+2],v=mk[i+3],k=lane.get(id);if(k===undefined)continue;const y=TOP+k*RH,tk=byId.get(id);\n",
    "      if(c===4){if(t<a||t>b)continue;misses++;if(!showEvents.checked)continue;const X=x(t),Y=y+RH/2,d='M'+(X-5)+' '+(Y-5)+'L'+(X+5)+' '+(Y+5)+'M'+(X+5)+' '+(Y-5)+'L'+(X-5)+' '+(Y+5);\n",
    "        const g=S('g',{class:'miss','data-tip':'T'+id+' FALLA de plazo en tick '+t},svg);S('path',{d:d,class:'ring'},g);S('path',{d:d},g);S('circle',{cx:X,cy:Y,r:9,fill:'transparent'},g);continue;}\n",
    "      if(!showEvents.checked)continue;\n",
    "      if(c===5){if(t<a||t>b)continue;const X=x(t),Y=y+RH/2;\n",
    "        S('path',{class:'ign',d:'M'+X+' '+(Y-6)+'L'+(X+6)+' '+Y+'L'+X+' '+(Y+6)+'L'+(X-6)+' '+Y+'Z','data-tip':'Pulsación ignorada en tick '+t+': '+(v===1?'el nodo no tiene tarea esporádica':'no pasó el intervalo mínimo'+(tk?' T='+tk.T:''))},svg);continue;}\n",
    "      if(c!==1||dense)continue;\n",
    "      if(t>=a&&t<=b){const X=x(t);S('path',{class:'rel',d:'M'+(X-4)+' '+(y+RH-1)+'L'+(X+4)+' '+(y+RH-1)+'L'+X+' '+(y+RH-8)+'Z','data-tip':'T'+id+(tk&&tk.ty==='S'?' (esporádica)':'')+' liberada en tick '+t+(tk?' · plazo '+(t+tk.D):'')},svg);}\n",
    "      if(tk){const d=t+tk.D;if(d>=a&&d<=b){const X=x(d);S('path',{class:'dl',d:'M'+(X-4)+' '+(y+2)+'L'+(X+4)+' '+(y+2)+'L'+X+' '+(y+8)+'Z','data-tip':'T'+id+' plazo absoluto en tick '+d+' (liberada en '+t+')'},svg);}}\n",
    "    }\n",
    "    info.textContent='Ticks '+a+'–'+b+' de '+end+' · '+(fine?'1 celda = 1 tick':'vista resumida')+' · fallas en la ventana: '+misses+(dense?' · acerque la ventana a ≤'+DENSE+' ticks para ver liberaciones y plazos':'');\n",
    "  }\n",
    "  window.addEventListener('resize',draw);\n",
    "  set(0);\n",
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
    "const last=new Map();for(const d of R.dec)last.set(d.id,d);\n",
    "const t0=R.dec.length?R.dec[0].ns:0;\n",
    "const finals=[...last.values()].sort(function(p,q){return p.id-q.id;});\n",
    "const nAcc=finals.filter(function(d){return d.k===1;}).length,nRej=finals.filter(function(d){return d.k===3;}).length;\n",
    "const adm=document.getElementById('admit');\n",
    "adm.append(H('p',null,nAcc+' aceptada(s) · '+nRej+' rechazada(s) · '+(finals.length-nAcc-nRej)+' en otro estado. Estado final de cada tarea:'),\n",
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

/* Página autocontenida: los datos van en `R` y el script dibuja un Gantt
 * por nodo con ventana desplazable (miles de ticks no caben en un SVG fijo). */
static void write_html(FILE *html,FILE *gantt,FILE *marks,FILE *decisions,const Stat *stats,size_t n,
                       const long double theoretical[NODES],const uint64_t busy[NODES],const uint64_t end_tick[NODES],
                       const uint64_t switches[NODES],uint32_t counters[NODES][5],const bool incomplete[NODES],
                       const uint64_t dropped[3]) {
    put_lines(html,PAGE_HEAD,sizeof PAGE_HEAD/sizeof *PAGE_HEAD);
    fprintf(html,"<script>\nconst R={nodes:['ESP32','Pico'],drop:%"PRIu64",\nutil:[",dropped[2]);
    for(int i=0;i<NODES;i++) {
        fprintf(html,"{th:%.6Lf,obs:%.6Lf,part:%d,sw:%"PRIu64",end:%"PRIu64",drop:%"PRIu64",cnt:[%u,%u,%u,%u,%u]},",
                theoretical[i],end_tick[i]?(long double)busy[i]/end_tick[i]:0.0L,incomplete[i],switches[i],end_tick[i],dropped[i],
                counters[i][0],counters[i][1],counters[i][2],counters[i][3],counters[i][4]);
    }
    fputs("],\ntasks:[\n",html);
    for(size_t i=0;i<n;i++) {
        const Stat *p=&stats[i];
        fprintf(html,"{n:%d,id:%"PRIu32",ty:'%c',W:%"PRIu32",T:%"PRIu32",D:%"PRIu32",C:%"PRIu32",rel:%"PRIu64",run:%"PRIu64",comp:%"PRIu64",miss:%"PRIu64",ign:%"PRIu64",f:",
                p->node,p->id,p->task.type=='S'?'S':'P',p->task.W,p->task.T,p->task.D,p->cost,p->releases,p->runs,p->completed,p->misses,p->ignored);
        if(p->first_n)fprintf(html,"%.3Lf",(long double)p->first_sum/p->first_n);
        else fputs("null",html);
        fputs(",c:",html);
        if(p->finish_n)fprintf(html,"%.3Lf,cm:%"PRIu64,(long double)p->finish_sum/p->finish_n,p->finish_max);
        else fputs("null,cm:null",html);
        fputs("},\n",html);
    }
    /* iv[nodo] = [id,inicio,fin,...]; mk[nodo] = [código,id,tick,valor,...]. */
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
    /* Marcas del Gantt y decisiones de admisión, para el HTML final. */
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
        if(r.kind==R_TASK) {
            fprintf(decisions,"{ns:%"PRIu64",n:%d,id:%"PRIu32",k:%d,ty:'%c',W:%"PRIu32",T:%"PRIu32",D:%"PRIu32",cr:%"PRId32",C:%"PRIu32",why:",
                    r.ns,r.node,r.task.id,r.code,r.task.type=='S'?'S':'P',r.task.W,r.task.T,r.task.D,r.task.criticality,r.value);
            js_text(decisions,r.text);
            fputs("},\n",decisions);
        }
        if(r.node<0 || r.node>=NODES)continue;
        int node=r.node;
        Trace *trace=&traces[node];
        if(r.kind==R_TICK) {
            /* tick k se reporta después de procesar el tramo [k,k+1).
             * ESP32 puede intercalar este reporte entre lotes de eventos más
             * antiguos: no se avanza el cursor del Gantt hasta recibirlos. */
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
        if(r.kind==R_STOP) {
            /* ACK de STOP llega después de todos los eventos pendientes.
             * Cerrar solo hasta un límite efectivamente observado. */
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
        if(r.kind==R_TASK) {
            Stat *st=find_stat(stats,&n,r.task.id,node);
            if(!st) {
                incomplete[node]=true;
                continue;
            }
            st->task=r.task;
            st->cost=r.value;
            /* code 1: confirmada; code 2: retirada al perder nodo. */
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
        /* SPORADIC_IGNORED con id 0: pulsación en un nodo sin tarea S. */
        if(r.code==RT_RELEASE || r.code==RT_MISS || r.code==RT_SPORADIC_IGNORED)
            fprintf(marks,"%d %d %"PRIu32" %"PRIu32" %"PRIu32"\n",node,r.code,r.id,r.tick,r.value);
        if(!r.id || r.code==RT_TASK_ADDED)continue;
        Stat *st=find_stat(stats,&n,r.id,node);
        if(!st) {
            incomplete[node]=true;
            continue;
        }
        if(r.code==RT_RELEASE) {
            st->releases++;
            st->ambiguous=st->active;
            st->release=r.tick;
            st->job=r.value;
            st->active=true;
            st->first=false;
        } else if(r.code==RT_COMPLETE) {
            st->completed++;
            if(st->active && st->job==r.value && !st->ambiguous && r.tick>=st->release) {
                uint64_t response=r.tick-st->release;
                st->finish_sum+=response;
                st->finish_n++;
                if(response>st->finish_max)st->finish_max=response;
            }
            st->active=false;
        } else if(r.code==RT_MISS) {
            st->misses++;
            st->active=false;
        } else if(r.code==RT_SPORADIC_IGNORED)st->ignored++;
    }
    uint64_t dropped[3]= {
        0
    };
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
