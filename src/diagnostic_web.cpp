#include "diagnostic_web.h"

#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <sys/time.h>
#include <time.h>

#ifndef VOLVO_MDI_TEST_MODE
#define VOLVO_MDI_TEST_MODE 0
#endif

namespace {

constexpr char kPersistentCapturePath[] = "/auto_capture_bits.csv";
constexpr char kPreviousCapturePath[] = "/auto_capture_bits.previous.csv";
constexpr char kResearchCapturePath[] = "/research_events.csv";
// Keep two bounded generations while reserving ample room for SPIFFS garbage
// collection and SensESP configuration files. The previous 384 KiB limit left
// only one physically free block after sustained capture, which could make a
// WiFi settings write fail despite apparently available logical space.
// Two generations must fit with room to spare: SPIFFS needs whole free blocks
// for garbage collection and starts refusing writes well before the nominal
// capacity is reached. A device recovered from a full filesystem showed 0 of
// 224 free 4 KiB blocks with 2 x 384 KiB of capture on an 896 KiB partition.
// The partition is now 512 KiB, so 2 x 128 KiB leaves roughly half of it free.
constexpr size_t kPersistentCaptureMaxBytes = 128U * 1024U;
// Never let the capture take the last of the filesystem: the SensESP WiFi and
// Signal K configuration files must always be rewritable.
constexpr size_t kPersistentReservedBytes = 64U * 1024U;
// Hard ceiling on how often one PGN/source/subtype may write a row. Counter
// bytes we have not identified yet cannot turn into a flash write storm.
constexpr uint32_t kPersistentMinRowIntervalMs = 1000;
constexpr char kPersistentCaptureHeader[] =
    "boot_id,utc,epoch_ms,uptime_ms,event,can_id_hex,pgn,pgn_hex,source_hex,"
    "length,raw_changed_mask,trigger_mask,xor_hex,data_hex\r\n";
constexpr char kResearchCaptureHeader[] =
    "boot_id,utc,epoch_ms,uptime_ms,event,hypothesis_id,code,label,"
    "confidence,response,pgn,data_hex\r\n";

const char kDashboard[] PROGMEM = R"HTML(<!doctype html><html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Volvo MDI · Diagnóstico</title><style>
:root{color-scheme:dark;--bg:#081116;--card:#102129;--line:#24404c;--ink:#e8f5f7;--muted:#8ba8b1;--cyan:#4ed7dc;--green:#61dc94;--red:#ff6b72;--amber:#ffc857}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 80% 0,#12313b 0,#081116 42%);color:var(--ink);font:14px system-ui,sans-serif}main{max-width:1400px;margin:auto;padding:18px}.top{display:flex;gap:16px;align-items:center;justify-content:space-between}.brand{font-size:22px;font-weight:750}.sub,.muted{color:var(--muted)}.pill{border:1px solid var(--line);border-radius:99px;padding:7px 11px}.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;margin:16px 0}.card{background:#102129e8;border:1px solid var(--line);border-radius:14px;padding:14px;box-shadow:0 8px 24px #0004}.label{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em}.value{font-size:24px;font-weight:700;margin-top:4px}.ok{color:var(--green)}.bad{color:var(--red)}.warn{color:var(--amber)}h2{font-size:16px;margin:0 0 12px}.toolbar{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:12px}button,a.btn,input{appearance:none;border:1px solid #37606f;background:#16313c;color:var(--ink);border-radius:9px;padding:8px 12px;text-decoration:none}button,a.btn{cursor:pointer}button.primary,a.primary{background:#12676c;border-color:#26aeb5}.marker-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px;margin-top:12px}.marker{display:flex;align-items:center;justify-content:space-between;gap:8px;min-height:46px;text-align:left}.marker b{font-size:11px;color:var(--muted)}.marker.active{background:#742b32;border-color:var(--red);box-shadow:inset 0 0 0 1px #ff6b7266}.marker.active b{color:#fff}.marker:disabled{opacity:.45;cursor:not-allowed}.tablewrap{overflow:auto;max-height:58vh}table{width:100%;border-collapse:collapse;white-space:nowrap}th,td{text-align:left;border-bottom:1px solid var(--line);padding:9px 8px;font-variant-numeric:tabular-nums}th{position:sticky;top:0;background:var(--card);color:var(--muted);font-size:11px;text-transform:uppercase}.bytes{font-family:ui-monospace,monospace;color:#bcecef}.conversion{white-space:normal;min-width:330px}.foot{margin:12px 0;color:var(--muted);font-size:12px}@media(max-width:900px){.grid,.marker-grid{grid-template-columns:repeat(2,1fr)}}@media(max-width:520px){main{padding:12px}.grid{grid-template-columns:1fr 1fr}.marker-grid{grid-template-columns:1fr}.value{font-size:19px}.top{align-items:flex-start;flex-direction:column}}
</style></head><body><main><div class="top"><div><div class="brand">VOLVO MDI · DIAGNÓSTICO</div><div class="sub">J1939 → Signal K · solo escucha CAN</div></div><div class="toolbar"><a class="btn primary" href="/research">Investigación</a><div class="pill" id="mode">…</div></div></div><section class="grid" id="cards"></section><section class="card"><h2>Captura automática persistente</h2><div class="toolbar"><a class="btn primary" href="/auto-capture.csv">Descargar cambios CSV</a><a class="btn" href="/auto-capture-previous.csv">Descargar anterior</a><button onclick="clearAuto()">Borrar capturas automáticas</button></div><span id="autoCapture" class="muted">Siempre activa · conserva cambios entre reinicios</span><div class="foot">Registra línea base y cambios de bytes de cada PGN. RPM (EEC1 bytes 3–4) y el PGN de temperatura quedan fuera del disparador; la trama completa se conserva cuando cambia otro campo. Borrar elimina sólo estos dos CSV; no toca WiFi, Signal K ni investigación.</div></section><section class="card" style="margin-top:12px"><h2>Actualizar firmware por WiFi</h2><div class="toolbar"><input id="firmwareFile" type="file" accept=".bin,application/octet-stream"><input id="firmwarePassword" type="password" placeholder="Clave del AP / OTA"><button onclick="checkFirmwarePassword()">Comprobar clave</button><button class="primary" onclick="uploadFirmware()">Subir e instalar</button></div><span id="firmwareStatus" class="muted">Selecciona el firmware.bin compilado para LIVE.</span><div class="foot">Comprueba la clave antes de transferir. Durante la subida muestra el progreso real en tramos del 10 %. No cortes la alimentación durante la actualización.</div></section><section class="card" style="margin-top:12px"><h2>Velocidad del bus CAN</h2><div class="toolbar"><button onclick="forgetBitrate()">Olvidar velocidad aprendida y reiniciar</button></div><span id="canStatus" class="muted">La velocidad se aprende una sola vez y despues no vuelve a cambiar sola.</span><div class="foot">Usalo solo si sospechas que la velocidad guardada es incorrecta. Antes obligaba a borrar toda la flash, lo que tambien destruia la configuracion WiFi y de Signal K.</div></section><section class="card" style="margin-top:12px"><h2>Captura manual y observaciones</h2><div class="toolbar"><button class="primary" onclick="cmd('start')">Iniciar captura</button><button onclick="cmd('stop')">Detener</button><button onclick="cmd('clear')">Vaciar</button><a class="btn" href="/capture.csv">Descargar CSV manual</a></div><span id="capture" class="muted">…</span><div class="marker-grid" id="markers"></div><div class="foot">Pulsa una alarma cuando se encienda y vuelve a pulsarla cuando se apague. Ambos cambios quedan marcados junto a las tramas.</div></section><section class="card" style="margin-top:12px"><h2>Tramas J1939 recibidas y conversión</h2><div class="tablewrap"><table><thead><tr><th>PGN</th><th>Nombre</th><th>Origen</th><th>Hz</th><th>Datos</th><th>Conversión / destino Signal K</th><th>Edad</th></tr></thead><tbody id="rows"></tbody></table></div></section><div class="foot">El panel trabaja en modo CAN de solo escucha. Los marcadores son observaciones manuales y no se envían a CAN ni a Signal K.</div></main><script>
let csrfToken='';const markerNames=['Temperatura alta','Presión de aceite','Voltaje / batería','Check / avería motor','Fallo calentadores','Fallo sistema MDI','Fallo auxiliar MDI'];
const manualPanel=document.createElement('section');manualPanel.className='card';manualPanel.style.marginTop='12px';manualPanel.innerHTML='<h2>Reglas experimentales para investigación</h2><div id="manualLights" class="grid" style="margin:0"></div><div class="foot">Solo ayudan a comparar el manual con el tráfico real. No se publican en Signal K, no gobiernan las luces físicas y no transmiten nada al CAN.</div>';document.getElementById('cards').insertAdjacentElement('afterend',manualPanel);
function manualRuleCard(name,r,detail){let state=!r.valid?'SIN DATOS':r.on?'ENCENDIDA':r.condition?'TEMPORIZANDO':'APAGADA',tone=!r.valid?'warn':r.on?'bad':r.condition?'warn':'ok',timing=r.valid&&r.condition&&!r.on?' · '+(r.elapsedMs/1000).toFixed(1)+'/'+(r.thresholdMs/1000).toFixed(1)+' s':'',changed=r.changedMs?' · cambio t+'+(r.changedMs/1000).toFixed(1)+' s':'';return `<div class=card><div class=label>${name}</div><div class="value ${tone}">${state}</div><div class=muted>${detail}${timing}${changed}</div></div>`}
async function refreshManualRules(){try{let r=await fetch('/api/status',{cache:'no-store'}),s=await r.json(),m=s.manualRules,l=document.getElementById('manualLights');l.innerHTML=manualRuleCard('Aceite',m.oil,'&lt;60 kPa · 30 s bajo 1000 rpm / 0,5 s desde 1000')+manualRuleCard('Temperatura',m.temperature,'&gt;110 °C durante 15 s')+manualRuleCard('Tensión baja',m.lowVoltage,'entrada MDI ≤13 V durante 10 s · motor en marcha')+manualRuleCard('Tensión alta',m.highVoltage,'entrada MDI ≥15 V durante 30 s')}catch(e){document.getElementById('manualLights').innerHTML='<div class="bad">Sin respuesta del ESP32</div>'}}refreshManualRules();setInterval(refreshManualRules,500);
const fmt=(v,d=1)=>v==null?'—':Number(v).toFixed(d),cls=v=>v?'ok':'bad';async function refresh(){try{let [s,f]=await Promise.all([fetch('/api/status',{cache:'no-store'}),fetch('/api/frames',{cache:'no-store'})]);s=await s.json();f=await f.json();let alarm=s.alarms.overTemperature||s.alarms.lowOil||s.alarms.lowVoltage||s.alarms.engineCheck,alarmValid=s.alarms.dataFresh,valid=s.can.frames>0;mode.textContent=s.mode+' · núcleo web '+s.webCore;cards.innerHTML=`<div class=card><div class=label>Signal K</div><div class="value ${cls(s.signalk.connected)}">${s.signalk.status}</div><div class=muted>${s.signalk.server}</div></div><div class=card><div class=label>CAN / J1939</div><div class="value ${cls(s.can.started||s.mode==='TEST')}">${s.can.bitrate} kbps</div><div class=muted>${s.can.listenOnly?'LISTEN ONLY · TX deshabilitado':'Atención'} · ${s.can.bitrateSaved?'velocidad guardada':'detección inicial'} · SA ${s.can.source}</div></div><div class=card><div class=label>Tramas J1939</div><div class="value ${valid?'ok':'warn'}">${s.can.frames}</div><div class=muted>${s.can.unknown} sin decodificar · último PGN ${s.can.latestPgn||'—'}</div></div><div class=card><div class=label>Motor</div><div class=value>${fmt(s.engine.rpm,0)} rpm</div><div class=muted>${fmt(s.engine.coolantC)} °C · ${fmt(s.engine.oilKpa,0)} kPa · ${fmt(s.engine.voltage)} V</div></div><div class=card><div class=label>Alarmas decodificadas</div><div class="value ${!alarmValid?'warn':alarm?'bad':'ok'}">${!alarmValid?'SIN DATOS':alarm?'ACTIVA':'Sin alarmas'}</div><div class=muted>${alarmValid?'temp '+(s.alarms.overTemperature?'ON':'off')+' · aceite '+(s.alarms.lowOil?'ON':'off')+' · tensión '+(s.alarms.lowVoltage?'ON':'off')+' · check '+(s.alarms.engineCheck?'ON':'off'):'Último estado obsoleto; no se interpreta como normal'}</div></div><div class=card><div class=label>Volvo MDI</div><div class="value ${s.mdi.detected?'ok':'warn'}">${s.mdi.detected?'Detectado':'No detectado'}</div><div class=muted>DM1 ${s.mdi.dm1Available?'tramas recientes':'sin datos recientes'} · mapa propietario ${s.mdi.mappingVerified?'verificado':'no verificado'} · heap ${Math.round(s.heap/1024)} KiB</div></div>`;autoCapture.textContent=s.autoCapture.ready?(s.autoCapture.full?'PAUSADA · flash sin espacio':'ACTIVA')+' · '+s.autoCapture.rows+' cambios · '+Math.round(s.autoCapture.bytes/1024)+' KiB · '+Math.round((s.autoCapture.freeBytes||0)/1024)+' KiB libres'+(s.autoCapture.dropped?' · '+s.autoCapture.dropped+' perdidos':''):'No disponible';autoCapture.className=s.autoCapture.full?'bad':'muted';capture.textContent=(s.capture.active?'CAPTURANDO':'Detenida')+' · '+s.capture.rows+'/160 muestras'+(s.capture.active?' · t+'+Math.round(s.capture.elapsedMs/100)/10+' s':'');markers.innerHTML=markerNames.map((n,i)=>{let on=!!s.capture.markers[i];return `<button class="marker ${on?'active':''}" ${s.capture.active?'':'disabled'} onclick="toggleMarker(${i},${on?0:1})"><span>${n}</span><b>${on?'ACTIVA':'inactiva'}</b></button>`}).join('');rows.innerHTML=f.frames.map(x=>`<tr><td>${x.pgn}<br><span class=muted>0x${x.pgnHex}</span></td><td>${x.name}</td><td>0x${x.source}</td><td>${x.hz}</td><td class=bytes>${x.data}</td><td class=conversion>${x.conversion}</td><td>${x.ageMs} ms</td></tr>`).join('')||'<tr><td colspan=7 class=muted>Aún no se han recibido tramas J1939.</td></tr>'}catch(e){cards.innerHTML='<div class="card bad">Sin respuesta del ESP32</div>'}}
window.cmd=async c=>{capture.textContent='Enviando orden…';try{let r=await fetch('/api/capture/'+c,{method:'POST',cache:'no-store',headers:{'X-MDI-CSRF':csrfToken}});if(!r.ok)throw new Error('HTTP '+r.status);let j=await r.json();if(j.ok===false)throw new Error(j.error||'orden rechazada');await refresh()}catch(e){capture.textContent='Error de captura: '+e.message}};
window.toggleMarker=async(i,on)=>{try{let r=await fetch('/api/marker/'+i+'/'+(on?'on':'off'),{method:'POST',cache:'no-store',headers:{'X-MDI-CSRF':csrfToken}});let j=await r.json();if(!r.ok||j.ok===false)throw new Error(j.error||'HTTP '+r.status);await refresh()}catch(e){capture.textContent='Error de marcador: '+e.message}};async function init(){try{let r=await fetch('/api/session',{cache:'no-store'});let j=await r.json();csrfToken=j.token||'';await refresh();setInterval(refresh,1000)}catch(e){cards.innerHTML='<div class="card bad">No se pudo iniciar una sesión segura</div>'}}init();
window.forgetBitrate=async()=>{if(!confirm('¿Olvidar la velocidad CAN aprendida y reiniciar? El ESP32 volverá a probar 250/500 kbps. No afecta a la WiFi ni a Signal K.'))return;try{let r=await fetch('/api/can/forget-bitrate',{method:'POST',cache:'no-store',headers:{'X-MDI-CSRF':csrfToken}}),j=await r.json();if(!r.ok||j.ok===false)throw new Error(j.error||'HTTP '+r.status);canStatus.textContent='Velocidad borrada. Reiniciando…'}catch(e){canStatus.textContent='Error: '+e.message}};
window.clearAuto=async()=>{if(!confirm('¿Borrar la captura automática actual y la anterior? Las credenciales WiFi, Signal K y la investigación se conservarán.'))return;autoCapture.textContent='Borrando únicamente los CSV automáticos…';try{let r=await fetch('/api/auto-capture/clear',{method:'POST',cache:'no-store',headers:{'X-MDI-CSRF':csrfToken}}),j=await r.json();if(!r.ok||j.ok===false)throw new Error(j.error||'HTTP '+r.status);await refresh()}catch(e){autoCapture.textContent='No se pudo borrar: '+e.message}};
window.checkFirmwarePassword=async()=>{let p=firmwarePassword.value;if(!p){firmwareStatus.textContent='Introduce la clave del AP / OTA';return false}firmwareStatus.textContent='Comprobando clave…';let r;try{r=await fetch('/api/firmware/check-password',{method:'POST',cache:'no-store',headers:{'X-MDI-CSRF':csrfToken,'X-MDI-OTA-Password':p}});let body=await r.text(),j={};if(body){try{j=JSON.parse(body)}catch(e){if(!r.ok)throw e}}if(!r.ok||j.ok===false)throw new Error(j.error||'clave incorrecta');firmwareStatus.textContent='Clave válida ✓';return true}catch(e){firmwareStatus.textContent=r&&r.status===403?'Clave incorrecta ✗':'No se pudo comprobar la clave: '+e.message;return false}};
window.uploadFirmware=async()=>{let f=firmwareFile.files[0],p=firmwarePassword.value;if(!f){firmwareStatus.textContent='Selecciona primero firmware.bin';return}if(!await checkFirmwarePassword())return;if(!confirm('Clave válida. ¿Instalar '+f.name+' y reiniciar el ESP32?')){firmwareStatus.textContent='Clave válida ✓ · actualización cancelada';return}firmwareStatus.textContent='Preparando subida… 0%';let last=-10,x=new XMLHttpRequest();x.open('POST','/api/firmware');x.setRequestHeader('Content-Type','application/octet-stream');x.setRequestHeader('X-MDI-CSRF',csrfToken);x.setRequestHeader('X-MDI-OTA-Password',p);x.upload.onprogress=e=>{if(!e.lengthComputable)return;let n=Math.min(90,Math.floor(e.loaded/e.total*10)*10);if(n>=last+10){last=n;firmwareStatus.textContent='Subiendo '+Math.round(f.size/1024)+' KiB… '+n+'%'}};x.onerror=()=>firmwareStatus.textContent='Actualización fallida: conexión interrumpida';x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch(e){}if(x.status<200||x.status>=300||j.ok===false){firmwareStatus.textContent=x.status===403?'Clave rechazada ✗':'Actualización fallida: '+(j.error||'HTTP '+x.status);return}firmwareStatus.textContent='100% · firmware instalado. El ESP32 se está reiniciando…';firmwarePassword.value=''};x.send(f)};
</script></body></html>)HTML";

const char kResearchPage[] PROGMEM = R"HTML(<!doctype html><html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Volvo MDI · Investigación</title><style>
:root{color-scheme:dark;--bg:#081116;--card:#102129;--line:#24404c;--ink:#e8f5f7;--muted:#8ba8b1;--cyan:#4ed7dc;--green:#61dc94;--red:#ff6b72;--amber:#ffc857}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 80% 0,#12313b 0,#081116 42%);color:var(--ink);font:15px system-ui,sans-serif}main{max-width:900px;margin:auto;padding:18px}.top,.toolbar{display:flex;gap:10px;align-items:center;justify-content:space-between;flex-wrap:wrap}.brand{font-size:22px;font-weight:750}.muted{color:var(--muted)}.card{background:#102129e8;border:1px solid var(--line);border-radius:14px;padding:16px;margin-top:14px;box-shadow:0 8px 24px #0004}h2{font-size:16px;margin:0 0 12px}.guess{font-size:20px;font-weight:700;color:var(--amber);margin:8px 0}.evidence{font-family:ui-monospace,monospace;color:#bcecef;word-break:break-word}button,a.btn{appearance:none;border:1px solid #37606f;background:#16313c;color:var(--ink);border-radius:9px;padding:11px 14px;text-decoration:none;cursor:pointer;touch-action:manipulation;-webkit-user-select:none;user-select:none}button.yes{background:#17613c;border-color:var(--green)}button.no{background:#67262d;border-color:var(--red)}button:disabled{opacity:.4;cursor:not-allowed}.marks{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:9px}.marks button{min-height:58px;font-size:16px;font-weight:700}.marks button.marking{background:#17613c;border-color:var(--green)}.status{border-radius:10px;padding:10px;background:#0b1a20;color:var(--muted)}.manual-log{margin-top:10px;line-height:1.7}details.card>summary{cursor:pointer;font-weight:700;color:var(--muted);touch-action:manipulation}details[open]>summary{color:var(--ink);margin-bottom:12px}.foot{margin-top:12px;color:var(--muted);font-size:12px}@media(max-width:560px){main{padding:12px}.marks{grid-template-columns:1fr 1fr}.marks button{font-size:14px;min-height:62px}.guess{font-size:18px}}
</style></head><body><main><div class="top"><div><div class="brand">VOLVO MDI · INVESTIGACIÓN</div><div class="muted">Registro manual preciso · CAN siempre en solo escucha</div></div><a class="btn" href="/">Volver a diagnóstico</a></div>
<section class="card"><h2>Registrar lo que ocurre</h2><div class="marks" id="marks"></div><div id="save" class="foot">Sincronizando el reloj del teléfono con el ESP32…</div><div id="manualLog" class="manual-log muted">Todavía no has marcado ningún acontecimiento.</div><div class="foot">Durante la maniobra usa solamente estos botones. Cada toque es un hecho observado y no necesita confirmación posterior.</div></section>
<section class="card"><h2>Última detección automática, sólo informativa</h2><div id="guess" class="guess">Esperando una transición…</div><div id="detail" class="status">Las detecciones automáticas se guardan solas. No tienes que confirmarlas mientras manejas el motor.</div></section>
<details class="card"><summary>Revisar hipótesis automáticas después de la prueba</summary><div id="eventHistory" class="muted">Todavía no hay eventos.</div><div class="foot">Esta revisión es opcional. Cada tarjeta pregunta por un evento concreto e indica su tiempo; no la uses durante START o STOP.</div></details>
<section class="card"><h2>Datos de investigación</h2><div class="toolbar" style="justify-content:flex-start"><a class="btn" href="/research.csv">Descargar confirmaciones CSV</a><a class="btn" href="/auto-capture.csv">Descargar tramas CSV</a><a class="btn" href="/auto-capture-previous.csv">Descargar tramas anteriores</a></div><div id="stats" class="foot">…</div></section>
<script>
let csrf='',clockOffset=0,bestRtt=1e9,clockReady=false,manualEvents=[];
const observations=[[8,'ON · alimento el MDI'],[1,'Inicio de precalentamiento'],[2,'Fin de precalentamiento'],[3,'Inicio de pitidos'],[4,'Fin de pitidos'],[5,'Pulso START'],[6,'Fin del motor de arranque'],[9,'Motor funcionando'],[7,'Pulso STOP'],[10,'Motor detenido']];
marks.innerHTML=observations.map(x=>`<button disabled data-code="${x[0]}" onpointerdown="mark(${x[0]},this,event)" onkeydown="keyMark(${x[0]},this,event)">${x[1]}</button>`).join('');
function render(j){guess.textContent=j.label||'Esperando una transición…';detail.innerHTML=j.id?`Evento automático a t=${(j.uptimeMs/1000).toFixed(3)} s · PGN ${j.pgn||'—'} · confianza provisional ${j.confidence}%<br><span class=evidence>${j.data||''}</span>`:'Las detecciones automáticas se guardan solas. No tienes que confirmarlas mientras manejas el motor.';eventHistory.innerHTML=(j.events||[]).map(e=>`<div class=status style="margin-top:8px"><b>¿Ocurrió realmente: ${e.label}?</b><br>Detección a t=${(e.uptimeMs/1000).toFixed(3)} s · confianza ${e.confidence}% · <span class=evidence>${e.data}</span><br>${e.pending?`<button class=yes onclick="answerFor('yes',${e.id})">Sí, ocurrió</button> <button class=no onclick="answerFor('no',${e.id})">No ocurrió</button>`:e.response}</div>`).join('')||'Todavía no hay eventos.';stats.textContent=(j.rows||0)+' eventos/confirmaciones guardados · '+Math.round((j.bytes||0)/1024)+' KiB'}
async function refresh(){try{let t0=performance.now(),r=await fetch('/api/research',{cache:'no-store'}),j=await r.json(),t1=performance.now(),rtt=t1-t0;if(Number.isFinite(j.serverUptimeMs)&&rtt<bestRtt){bestRtt=rtt;clockOffset=j.serverUptimeMs-(t0+t1)/2;clockReady=true;document.querySelectorAll('#marks button').forEach(b=>b.disabled=false);save.textContent='Reloj sincronizado · precisión de red estimada ±'+Math.ceil(rtt/2)+' ms'}render(j)}catch(e){guess.textContent='Sin respuesta del ESP32';detail.textContent=e.message}}
async function post(path){let r=await fetch(path,{method:'POST',cache:'no-store',headers:{'X-MDI-CSRF':csrf}}),j=await r.json();if(!r.ok||j.ok===false)throw new Error(j.error||'HTTP '+r.status);await refresh()}
async function answerFor(v,id){try{await post('/api/research/respond/'+v+'/'+id)}catch(e){save.textContent='No se pudo guardar: '+e.message}}
function keyMark(i,b,e){if(e.repeat||(e.key!=='Enter'&&e.key!==' '))return;e.preventDefault();mark(i,b,e)}
async function mark(i,b,e){if(e)e.preventDefault();if(!clockReady){save.textContent='Espera a que termine la sincronización';return}let touched=performance.now(),eventMs=Math.round(touched+clockOffset)>>>0,label=observations.find(x=>x[0]===i)[1];b.classList.add('marking');if(navigator.vibrate)navigator.vibrate(25);save.textContent='Marca capturada localmente a t='+(eventMs/1000).toFixed(3)+' s · enviando…';try{let r=await fetch('/api/research/observe/'+i+'/'+eventMs,{method:'POST',cache:'no-store',headers:{'X-MDI-CSRF':csrf}}),j=await r.json();if(!r.ok||j.ok===false)throw new Error(j.error||'HTTP '+r.status);manualEvents.unshift({label,t:eventMs});manualEvents=manualEvents.slice(0,5);manualLog.innerHTML='<b>Últimas marcas:</b><br>'+manualEvents.map(x=>x.label+' · t='+(x.t/1000).toFixed(3)+' s').join('<br>');save.textContent='GUARDADA · '+label+' · respuesta '+Math.round(performance.now()-touched)+' ms'}catch(e){save.textContent='No se pudo guardar: '+e.message}finally{setTimeout(()=>b.classList.remove('marking'),180)}}
async function init(){try{let r=await fetch('/api/session',{cache:'no-store'}),j=await r.json();csrf=j.token||'';for(let i=0;i<5;i++)await refresh();setInterval(refresh,500)}catch(e){guess.textContent='No se pudo iniciar la sesión'}}init();
</script></main></body></html>)HTML";

const char* pgn_name(uint32_t pgn) {
  switch (pgn) {
    case 60160: return "TP.DT";
    case 60416: return "TP.CM";
    case 61444: return "EEC1 · régimen/par";
    case 65226: return "DM1 · diagnósticos";
    case 65252: return "Shutdown";
    case 65253: return "Horas motor";
    case 65262: return "Temperatura";
    case 65263: return "Presión fluidos";
    case 65271: return "Potencia eléctrica";
    case 65276: return "Nivel combustible";
    case 65417: return "Volvo MDI propietario";
    default: return "PGN no mapeado";
  }
}

constexpr uint8_t kMarkerCount = 7;
constexpr uint8_t kResearchCodeCount = 11;

const char* research_code_name(uint8_t code) {
  static const char* const names[kResearchCodeCount + 1] = {
      "unknown", "preheat_start", "preheat_end", "beep_start", "beep_end",
      "crank_start", "crank_end", "manual_stop", "mdi_power_on",
      "engine_running", "engine_stopped", "low_voltage_alarm"};
  return code <= kResearchCodeCount ? names[code] : names[0];
}

const char* research_label(uint8_t code) {
  static const char* const labels[kResearchCodeCount + 1] = {
      "Transición desconocida",
      "Primera actividad del MDI; no confirma precalentamiento",
      "Posible fin de precalentamiento",
      "Actividad A0 iniciada; posible pitido",
      "Actividad A0 terminada; posible fin de pitido",
      "Posible START e inicio del motor de arranque",
      "Posible fin del motor de arranque",
      "Posible pulsación de STOP manual",
      "Encendido manual del MDI",
      "Motor funcionando confirmado manualmente",
      "Motor detenido confirmado manualmente",
      "Alarma MDI de baja tensión/carga activa"};
  return code <= kResearchCodeCount ? labels[code] : labels[0];
}

uint8_t research_confidence(uint8_t code) {
  static const uint8_t values[kResearchCodeCount + 1] =
      {0, 100, 60, 70, 65, 90, 80, 80, 100, 100, 100, 99};
  return code <= kResearchCodeCount ? values[code] : 0;
}

const char* marker_name(uint8_t marker_id) {
  static const char* const names[kMarkerCount] = {
      "high_coolant_temperature", "low_oil_pressure", "low_voltage_battery",
      "engine_check_fault", "glow_plug_fault", "mdi_system_fault",
      "mdi_auxiliary_fault"};
  return marker_id < kMarkerCount ? names[marker_id] : "unknown_marker";
}

String hex_bytes(const uint8_t* data, uint8_t len) {
  String out;
  out.reserve(24);
  char byte_text[4];
  for (uint8_t i = 0; i < len; i++) {
    snprintf(byte_text, sizeof(byte_text), "%02X", data[i]);
    if (i) out += ' ';
    out += byte_text;
  }
  return out;
}

uint16_t le16(const uint8_t* d, uint8_t o) {
  return uint16_t(d[o]) | (uint16_t(d[o + 1]) << 8);
}

uint32_t le32(const uint8_t* d) {
  return uint32_t(d[0]) | (uint32_t(d[1]) << 8) |
         (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24);
}

String conversion_text(uint32_t pgn, const uint8_t* d, uint8_t len) {
  char out[430];
  switch (pgn) {
    case 61444:
      if (len >= 5 && le16(d, 3) < 0xfb00) snprintf(out, sizeof(out), "SPN 190: raw %u × 0.125 = %.1f rpm → propulsion.main.revolutions = %.3f Hz; par no exportado", le16(d, 3), le16(d, 3) * .125f, le16(d, 3) * .125f / 60.f);
      else if (len >= 5) snprintf(out, sizeof(out), "SPN 190 no disponible/reservado; no se publica");
      else snprintf(out, sizeof(out), "Longitud insuficiente para EEC1");
      break;
    case 65253:
      if (len >= 4 && le32(d) < 0xfb000000UL) snprintf(out, sizeof(out), "SPN 247: raw %lu × 0.05 = %.2f h → propulsion.main.runTime = %.0f s", (unsigned long)le32(d), le32(d) * .05f, le32(d) * 180.f);
      else if (len >= 4) snprintf(out, sizeof(out), "SPN 247 no disponible/reservado; no se publica");
      else snprintf(out, sizeof(out), "Longitud insuficiente para SPN 247");
      break;
    case 65262:
      if (len >= 1 && d[0] < 0xfb) snprintf(out, sizeof(out), "SPN 110: raw %u − 40 = %.1f °C → propulsion.main.coolantTemperature = %.2f K", d[0], float(d[0]) - 40.f, float(d[0]) + 233.15f);
      else if (len >= 1) snprintf(out, sizeof(out), "SPN 110 no disponible/reservado; no se publica");
      else snprintf(out, sizeof(out), "Longitud insuficiente para SPN 110");
      break;
    case 65263:
      if (len >= 4 && d[3] < 0xfb) snprintf(out, sizeof(out), "SPN 100: raw %u × 4 = %.0f kPa → propulsion.main.oilPressure = %.0f Pa", d[3], d[3] * 4.f, d[3] * 4000.f);
      else if (len >= 4) snprintf(out, sizeof(out), "SPN 100 no disponible/reservado; no se publica");
      else snprintf(out, sizeof(out), "Longitud insuficiente para SPN 100");
      break;
    case 65271: {
      if (len < 8) { snprintf(out, sizeof(out), "Longitud insuficiente para VEP1"); break; }
      String text;
      const uint16_t values[3] = {le16(d, 2), le16(d, 4), le16(d, 6)};
      const char* labels[3] = {"SPN 167 → propulsion.main.alternatorVoltage", "SPN 168 → electrical.batteries.start.voltage", "SPN 158 → propulsion.main.volvoMdi.supplyVoltage"};
      for (uint8_t i = 0; i < 3; i++) { if (i) text += "; "; text += labels[i]; if (values[i] < 0xfb00) { text += " = "; text += String(values[i] * .05f, 2); text += " V"; } else text += " no disponible; no se publica"; }
      strlcpy(out, text.c_str(), sizeof(out));
      break;
    }
    case 65276:
      if (len >= 2 && d[1] < 0xfb) snprintf(out, sizeof(out), "SPN 96: raw %u × 0.4 = %.1f %% → tanks.fuel.main.currentLevel = %.3f ratio", d[1], d[1]*.4f, d[1]*.004f);
      else if (len >= 2) snprintf(out, sizeof(out), "SPN 96 no disponible/reservado; no se publica");
      else snprintf(out, sizeof(out), "Longitud insuficiente para SPN 96");
      break;
    case 65226:
      if (len >= 6) { uint32_t spn=uint32_t(d[2])|(uint32_t(d[3])<<8)|((uint32_t(d[4])&0xe0)<<11); snprintf(out,sizeof(out),"DM1: lámparas raw 0x%02X%02X; primer DTC SPN %lu, FMI %u, OC %u → volvoMdi.activeDtcCount / firstDtcSpn / firstDtcFmi",d[0],d[1],(unsigned long)spn,d[4]&0x1f,d[5]&0x7f); }
      else snprintf(out, sizeof(out), "DM1 sin DTC de trama única o transportado por TP");
      break;
    case 65417:
      snprintf(out, sizeof(out), "%s", VOLVO_MDI_TEST_MODE ? "8 bytes raw; mapa sintético de TEST" : "Volvo multiplexado: C1 byte 3 bit 0x20 = alarma de baja tensión/carga verificada; los demás campos siguen raw");
      break;
    case 60416: snprintf(out, sizeof(out), "Control J1939 Transport Protocol; ensambla DM1 multipaquete, no publica directamente"); break;
    case 60160: snprintf(out, sizeof(out), "Datos J1939 Transport Protocol; ensambla DM1 multipaquete, no publica directamente"); break;
    default: snprintf(out, sizeof(out), "Sin conversión configurada; conservado como PGN/raw para análisis"); break;
  }
  return String(out);
}

void response_headers(WiFiClient& c, const char* status, const char* type) {
  c.printf("HTTP/1.1 %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'\r\n\r\n", status, type);
}

}  // namespace

void DiagnosticWeb::begin(uint16_t port, const char* firmware_password) {
  if (task_ != nullptr) return;
  port_ = port;
  const uint64_t random_token =
      (uint64_t(esp_random()) << 32) | uint64_t(esp_random());
  snprintf(csrf_token_, sizeof(csrf_token_), "%08lx%08lx",
           (unsigned long)(random_token >> 32),
           (unsigned long)(random_token & 0xffffffffUL));
  if (firmware_password != nullptr) {
    strlcpy(firmware_password_, firmware_password,
            sizeof(firmware_password_));
  }
  mutex_ = xSemaphoreCreateMutex();
  if (mutex_ == nullptr) {
    Serial.println("Diagnostic dashboard disabled: mutex allocation failed");
    return;
  }
  persistent_queue_ = xQueueCreate(kPersistentQueueSlots, sizeof(PersistentRow));
  if (persistent_queue_ == nullptr) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
    Serial.println("Diagnostic dashboard disabled: persistent queue allocation failed");
    return;
  }
  research_queue_ = xQueueCreate(kResearchQueueSlots, sizeof(ResearchRow));
  if (research_queue_ == nullptr) {
    vSemaphoreDelete(mutex_);
    vQueueDelete(persistent_queue_);
    mutex_ = nullptr;
    persistent_queue_ = nullptr;
    Serial.println("Diagnostic dashboard disabled: research queue allocation failed");
    return;
  }
  persistent_boot_id_ = esp_random();
  const BaseType_t created = xTaskCreatePinnedToCore(
      task_entry, "mdi-diagnostic-web", 12288, this, 1, &task_, 0);
  if (created != pdPASS) {
    vSemaphoreDelete(mutex_);
    vQueueDelete(persistent_queue_);
    vQueueDelete(research_queue_);
    mutex_ = nullptr;
    persistent_queue_ = nullptr;
    research_queue_ = nullptr;
    task_ = nullptr;
    Serial.println("Diagnostic dashboard disabled: task allocation failed");
  }
}

void DiagnosticWeb::set_can_bitrate_reset_handler(bool (*handler)()) {
  can_bitrate_reset_handler_ = handler;
}

void DiagnosticWeb::task_entry(void* parameter) {
  static_cast<DiagnosticWeb*>(parameter)->task_loop();
}

void DiagnosticWeb::task_loop() {
  begin_persistent_capture();
  server_ = new WiFiServer(port_);
  server_->begin();
  server_->setNoDelay(true);
  for (;;) {
    drain_persistent_queue();
    drain_research_queue();
    WiFiClient client = server_->available();
    if (client) serve_client(client);
    // Keep the input-to-timestamp path short while the research page is in
    // use. This task is low priority and remains isolated from CAN decoding.
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void DiagnosticWeb::set_runtime_state(const DiagnosticRuntimeState& state) {
  // Published outside the mutex so the decode path can read it without
  // blocking, and so it is still updated when the snapshot copy is skipped.
  persistent_engine_source_ = state.engine_source;
  if (mutex_ == nullptr || xSemaphoreTake(mutex_, 0) != pdTRUE) return;
  runtime_ = state;
  xSemaphoreGive(mutex_);
}

void DiagnosticWeb::note_j1939_frame(uint32_t can_id, uint32_t pgn,
                                      uint8_t source, const uint8_t* data,
                                      uint8_t len) {
  len = min<uint8_t>(len, 8);
  const uint32_t now = millis();
  // Persistent tracking is independent of the web snapshot mutex. A browser
  // refresh must never hide a short-lived transition during cranking/stopping.
  track_persistent_change(can_id, pgn, source, data, len, now);
  detect_research_hypothesis(pgn, source, data, len, now);
  // Diagnostics must never delay the engine path. If the web task is copying a
  // snapshot, omit this diagnostic sample; J1939 decoding still proceeds.
  if (mutex_ == nullptr || xSemaphoreTake(mutex_, 0) != pdTRUE) return;
  FrameSlot* slot = nullptr;
  FrameSlot* oldest = &frames_[0];
  for (auto& candidate : frames_) {
    if (candidate.used && candidate.pgn == pgn && candidate.source == source) { slot = &candidate; break; }
    if (!candidate.used && slot == nullptr) slot = &candidate;
    if (candidate.last_ms < oldest->last_ms) oldest = &candidate;
  }
  if (slot == nullptr) slot = oldest;
  if (!slot->used || slot->pgn != pgn || slot->source != source) {
    *slot = FrameSlot{};
    slot->used = true;
    slot->pgn = pgn;
    slot->source = source;
    slot->first_ms = now;
  }
  const bool changed = slot->len != len || memcmp(slot->captured_data, data, len) != 0;
  slot->can_id = can_id;
  slot->len = len;
  memcpy(slot->data, data, len);
  slot->last_ms = now;
  slot->count++;
  if (capture_enabled_ &&
      (changed || slot->last_capture_ms == 0 ||
       now - slot->last_capture_ms >= 1000)) {
    CaptureRow row;
    row.timestamp_ms = now;
    row.relative_ms = now - capture_started_ms_;
    row.session = capture_session_;
    row.can_id = can_id;
    row.pgn = pgn;
    row.source = source;
    row.len = len;
    memcpy(row.data, data, len);
    memcpy(slot->captured_data, data, len);
    slot->last_capture_ms = now;
    append_capture_row_locked(row);
  }
  xSemaphoreGive(mutex_);
}

void DiagnosticWeb::track_persistent_change(uint32_t can_id, uint32_t pgn,
                                             uint8_t source,
                                             const uint8_t* data, uint8_t len,
                                             uint32_t now) {
  if (persistent_queue_ == nullptr) return;
  // These Volvo proprietary messages are multiplexed. Byte 0 identifies the
  // submessage and byte 1 is a rolling counter. Compare each subtype with its
  // own previous value and do not let the counter alone trigger flash writes.
  const bool multiplexed_volvo_pgn = pgn == 65417 || pgn == 65420;
  const uint8_t subtype = multiplexed_volvo_pgn && len > 0 ? data[0] : 0;
  // Once the engine address is known, the proprietary records broadcast by
  // other nodes carry nothing the decoder will ever act on. The tachometer at
  // SA 0xF2 sends PGN 65417 with every byte constant except two free-running
  // counters, and on real traffic it accounted for 87 % of the rows that
  // survived every other filter. Keep it visible in the live frame table and
  // in the manual capture, but stop writing its heartbeat to flash.
  const uint8_t engine_source = persistent_engine_source_;
  const bool foreign_proprietary_record =
      multiplexed_volvo_pgn && engine_source != 0xff && source != engine_source;
  PersistentSlot* slot = nullptr;
  PersistentSlot* oldest = &persistent_slots_[0];
  for (auto& candidate : persistent_slots_) {
    if (candidate.used && candidate.pgn == pgn &&
        candidate.source == source && candidate.subtype == subtype) {
      slot = &candidate;
      break;
    }
    if (!candidate.used && slot == nullptr) slot = &candidate;
    if (candidate.last_ms < oldest->last_ms) oldest = &candidate;
  }
  if (slot == nullptr) slot = oldest;
  const bool baseline =
      !slot->used || slot->pgn != pgn || slot->source != source ||
      slot->subtype != subtype;
  uint8_t raw_changed_mask = 0;
  uint8_t xor_data[8] = {};
  if (baseline || slot->len != len) {
    raw_changed_mask = len >= 8 ? 0xff : uint8_t((1U << len) - 1U);
    if (!baseline) {
      for (uint8_t i = 0; i < len; i++) {
        xor_data[i] = data[i] ^ (i < slot->len ? slot->data[i] : 0);
      }
    }
  } else {
    for (uint8_t i = 0; i < len; i++) {
      xor_data[i] = slot->data[i] ^ data[i];
      if (xor_data[i] != 0) raw_changed_mask |= uint8_t(1U << i);
    }
  }

  uint8_t trigger_mask = raw_changed_mask;
  // SPN 190 occupies bytes 3-4 (zero based) in EEC1. Preserve the complete
  // EEC1 frame if another byte changes, but RPM alone does not create a row.
  if (pgn == 61444) trigger_mask &= uint8_t(~((1U << 3) | (1U << 4)));
  // Temperature changes must not fill SPIFFS, but the 50 C boundary controls
  // whether the first START/PREHEAT press energizes the glow plugs. Preserve
  // the first ET1 sample and each crossing as experiment context. SPN 110 raw
  // 0x5A is exactly 50 C (raw - 40).
  if (pgn == 65262) {
    bool crossed_preheat_boundary = false;
    if (!baseline && len > 0 && slot->len > 0 && data[0] < 0xFB &&
        slot->data[0] < 0xFB) {
      crossed_preheat_boundary =
          (data[0] < 0x5A) != (slot->data[0] < 0x5A);
    }
    trigger_mask = baseline ? raw_changed_mask
                            : (crossed_preheat_boundary ? uint8_t(1U) : 0U);
  }
  if (multiplexed_volvo_pgn) trigger_mask &= uint8_t(~(1U << 1));
  // A baseline is still recorded, so the CSV always shows the record existed.
  if (foreign_proprietary_record && !baseline) trigger_mask = 0;

  slot->used = true;
  slot->pgn = pgn;
  slot->source = source;
  slot->subtype = subtype;
  slot->len = len;
  slot->last_ms = now;
  memcpy(slot->data, data, len);
  if (trigger_mask == 0) return;
  // Rate limit only a repetition of the exact same trigger pattern. Observed
  // traffic has the tachometer at SA 0xF2 sending PGN 65417 with two
  // free-running counters and every other byte constant; after masking the
  // byte-1 counter it still produced 92 % of all rows, always through byte 3
  // alone. Any byte that was not part of the previous trigger is new
  // information and is written immediately, so an alarm transition is never
  // lost behind a counter tick.
  const bool same_pattern_as_before =
      !baseline && slot->last_row_ms != 0 && trigger_mask == slot->last_trigger_mask;
  if (same_pattern_as_before &&
      now - slot->last_row_ms < kPersistentMinRowIntervalMs) {
    return;
  }
  slot->last_row_ms = now;
  slot->last_trigger_mask = trigger_mask;

  PersistentRow row;
  row.uptime_ms = now;
  row.can_id = can_id;
  row.pgn = pgn;
  row.source = source;
  row.len = len;
  row.raw_changed_mask = raw_changed_mask;
  row.trigger_mask = trigger_mask;
  row.baseline = baseline;
  memcpy(row.data, data, len);
  memcpy(row.xor_data, xor_data, len);
  if (xQueueSend(persistent_queue_, &row, 0) != pdTRUE) persistent_dropped_++;
}

void DiagnosticWeb::detect_research_hypothesis(uint32_t pgn, uint8_t source,
                                                const uint8_t* data,
                                                uint8_t len, uint32_t now) {
  if (research_queue_ == nullptr || pgn != 65417 || len < 4) return;
  const uint8_t engine_source = persistent_engine_source_;
  // Before source auto-detection completes, the real captures identify the MDI
  // as SA 0x00. Never infer engine events from the tachometer heartbeat at F2.
  if ((engine_source != 0xff && source != engine_source) ||
      (engine_source == 0xff && source != 0x00)) return;

  if (!research_mdi_seen_) {
    research_mdi_seen_ = true;
    publish_research_hypothesis(1, pgn, data, len, now);
  }

  const uint8_t subtype = data[0];
  if (subtype == 0xA0) {
    const bool active = data[2] == 0xFA && data[3] == 0x9F;
    if (research_a0_seen_ && active != research_a0_beep_active_) {
      publish_research_hypothesis(active ? 3 : 4, pgn, data, len, now);
    }
    research_a0_seen_ = true;
    research_a0_beep_active_ = active;
  } else if (subtype == 0xB2) {
    const bool active = data[2] == 0x02 && data[3] == 0x01;
    if (research_b2_seen_) {
      if (active) {
        // B2 also becomes active as part of the charge-alarm sequence. Only
        // propose START while the verified C1 low-voltage bit is clear.
        const bool start_candidate = !research_c1_low_voltage_active_;
        if (start_candidate && !research_b2_crank_active_)
          publish_research_hypothesis(5, pgn, data, len, now);
        research_b2_crank_active_ = start_candidate;
      } else {
        if (research_b2_crank_active_)
          publish_research_hypothesis(6, pgn, data, len, now);
        research_b2_crank_active_ = false;
      }
    } else {
      research_b2_crank_active_ = false;
    }
    research_b2_seen_ = true;
  } else if (subtype == 0xC1) {
    const bool active = (data[3] & 0x20U) != 0;
    if (research_c1_seen_ && active &&
        !research_c1_low_voltage_active_) {
      publish_research_hypothesis(11, pgn, data, len, now);
    }
    research_c1_seen_ = true;
    research_c1_low_voltage_active_ = active;
  }
}

void DiagnosticWeb::publish_research_hypothesis(uint8_t code, uint32_t pgn,
                                                 const uint8_t* data,
                                                 uint8_t len, uint32_t now) {
  ResearchRow row;
  row.uptime_ms = now;
  row.hypothesis_id = ++research_next_hypothesis_id_;
  row.pgn = pgn;
  row.event = 0;
  row.code = code;
  row.len = min<uint8_t>(len, 8);
  memcpy(row.data, data, row.len);
  // Research is diagnostic-only. If its small queue is ever full, raw CAN
  // capture still continues and the engine decode path is never blocked.
  xQueueSend(research_queue_, &row, 0);
}

void DiagnosticWeb::drain_research_queue() {
  if (research_queue_ == nullptr) return;
  ResearchRow row;
  while (xQueueReceive(research_queue_, &row, 0) == pdTRUE) {
    // Human observations are already timestamped. Persist them here, outside
    // the HTTP request, but do not present them as pending machine hypotheses.
    if (row.event == 2) {
      append_research_row(row);
      continue;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    research_hypothesis_id_ = row.hypothesis_id;
    research_hypothesis_ms_ = row.uptime_ms;
    research_hypothesis_pgn_ = row.pgn;
    research_hypothesis_code_ = row.code;
    research_hypothesis_response_ = 0;
    research_hypothesis_len_ = row.len;
    memcpy(research_hypothesis_data_, row.data, row.len);
    ResearchItem& item = research_history_[research_history_head_];
    item = ResearchItem{};
    item.hypothesis_id = row.hypothesis_id;
    item.uptime_ms = row.uptime_ms;
    item.pgn = row.pgn;
    item.code = row.code;
    item.len = row.len;
    memcpy(item.data, row.data, row.len);
    research_history_head_ =
        (research_history_head_ + 1) % kResearchHistorySlots;
    if (research_history_count_ < kResearchHistorySlots)
      research_history_count_++;
    xSemaphoreGive(mutex_);
    append_research_row(row);
  }
}

bool DiagnosticWeb::respond_to_research_hypothesis(uint32_t hypothesis_id,
                                                    bool confirmed) {
  ResearchRow row;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  ResearchItem* matched = nullptr;
  for (auto& item : research_history_) {
    if (item.hypothesis_id == hypothesis_id) {
      matched = &item;
      break;
    }
  }
  if (hypothesis_id == 0 || matched == nullptr || matched->response != 0) {
    xSemaphoreGive(mutex_);
    return false;
  }
  matched->response = confirmed ? 1 : 2;
  if (hypothesis_id == research_hypothesis_id_)
    research_hypothesis_response_ = matched->response;
  row.uptime_ms = millis();
  row.hypothesis_id = matched->hypothesis_id;
  row.pgn = matched->pgn;
  row.event = 1;
  row.code = matched->code;
  row.response = matched->response;
  row.len = matched->len;
  memcpy(row.data, matched->data, row.len);
  xSemaphoreGive(mutex_);
  append_research_row(row);
  return true;
}

bool DiagnosticWeb::add_research_observation(uint8_t code,
                                              uint32_t event_uptime_ms) {
  if (code == 0 || code > kResearchCodeCount) return false;
  const uint32_t received_ms = millis();
  // A synchronized browser timestamp removes WiFi/HTTP transport time. Reject
  // stale or forged values and fall back to the instant the request arrived.
  const int32_t clock_error_ms =
      static_cast<int32_t>(event_uptime_ms - received_ms);
  if (event_uptime_ms == 0 || clock_error_ms < -2000 || clock_error_ms > 2000)
    event_uptime_ms = received_ms;
  ResearchRow row;
  row.uptime_ms = event_uptime_ms;
  row.event = 2;
  row.code = code;
  // Never wait for a flash open/write/flush in the button request.
  return research_queue_ != nullptr &&
         xQueueSend(research_queue_, &row, 0) == pdTRUE;
}

void DiagnosticWeb::serve_client(WiFiClient& client) {
  client.setTimeout(75);
  client.setNoDelay(true);
  // WiFiServer listens on both ESP32 interfaces. Accept requests addressed to
  // either the password-protected diagnostic AP or the station address on the
  // boat LAN, but reject traffic received on any unexpected interface.
  const IPAddress destination = client.localIP();
  const bool via_diagnostic_ap = destination == WiFi.softAPIP();
  const bool via_boat_lan =
      WiFi.status() == WL_CONNECTED && destination == WiFi.localIP();
  if (!via_diagnostic_ap && !via_boat_lan) {
    send_forbidden(client);
    client.stop();
    return;
  }
  const uint32_t deadline = millis() + 400;
  while (!client.available() && client.connected() && millis() < deadline) vTaskDelay(1);
  if (!client.available()) { client.stop(); return; }
  String request = client.readStringUntil('\n');
  request.trim();
  const bool get = request.startsWith("GET ");
  const bool post = request.startsWith("POST ");
  int path_start = request.indexOf(' ') + 1;
  int path_end = request.indexOf(' ', path_start);
  String path = path_end > path_start ? request.substring(path_start, path_end) : "";
  String csrf_header;
  String host_header;
  String origin_header;
  String firmware_password;
  size_t content_length = 0;
  while (client.connected() && millis() < deadline) {
    String header = client.readStringUntil('\n');
    header.trim();
    if (header.isEmpty()) break;
    if (header.startsWith("X-MDI-CSRF:")) {
      csrf_header = header.substring(11);
      csrf_header.trim();
    } else if (header.startsWith("Host:")) {
      host_header = header.substring(5);
      host_header.trim();
    } else if (header.startsWith("Origin:")) {
      origin_header = header.substring(7);
      origin_header.trim();
    } else if (header.startsWith("Content-Length:")) {
      content_length = size_t(header.substring(15).toInt());
    } else if (header.startsWith("X-MDI-OTA-Password:")) {
      firmware_password = header.substring(19);
      firmware_password.trim();
    }
  }
  const bool same_origin =
      origin_header.isEmpty() || origin_header == String("http://") + host_header;
  const bool authorized_mutation =
      post && same_origin && csrf_header == csrf_token_;
  bool reboot_after_response = false;
  if (get && path == "/") send_page(client);
  else if (get && path == "/research") send_research_page(client);
  else if (get && path == "/api/session") send_session(client);
  else if (get && path == "/api/status") send_status(client);
  else if (get && path == "/api/frames") send_frames(client);
  else if (get && path == "/api/research") send_research_status(client);
  else if (get && path == "/capture.csv") send_capture(client);
  else if (get && path == "/auto-capture.csv") send_persistent_capture(client);
  else if (get && path == "/auto-capture-previous.csv") send_persistent_capture(client, true);
  else if (get && path == "/research.csv") send_research_capture(client);
  else if (path == "/api/firmware/check-password") {
    if (!post) {
      send_method_not_allowed(client);
    } else if (!authorized_mutation || firmware_password.length() == 0 ||
               firmware_password != firmware_password_) {
      send_forbidden(client);
    } else {
      send_json_ok(client);
    }
  }
  else if (path == "/api/firmware") {
    if (!post) {
      send_method_not_allowed(client);
    } else if (!authorized_mutation || firmware_password.length() == 0 ||
               firmware_password != firmware_password_) {
      send_forbidden(client);
    } else if (content_length == 0) {
      send_json_result(client, false, "firmware vacío");
    } else {
      reboot_after_response = receive_firmware(client, content_length);
    }
  }
  else if (path == "/api/can/forget-bitrate") {
    // The learned bitrate is deliberately sticky: once a valid extended frame
    // has confirmed it, silence means a wiring or power problem rather than
    // permission to change timing. Clearing it used to require a full flash
    // erase, which also destroyed the WiFi configuration.
    if (!post) {
      send_method_not_allowed(client);
    } else if (!authorized_mutation) {
      send_forbidden(client);
    } else if (can_bitrate_reset_handler_ == nullptr) {
      send_json_result(client, false, "no disponible en este firmware");
    } else if (!can_bitrate_reset_handler_()) {
      send_json_result(client, false, "no se pudo borrar la velocidad guardada");
    } else {
      reboot_after_response = true;
      send_json_ok(client);
    }
  }
  else if (path == "/api/auto-capture/clear") {
    if (!post) {
      send_method_not_allowed(client);
    } else if (!authorized_mutation) {
      send_forbidden(client);
    } else if (!clear_persistent_capture()) {
      send_json_result(client, false, "no se pudieron recrear los CSV automáticos");
    } else {
      send_json_ok(client);
    }
  }
  else if (path == "/api/capture/start" ||
           path == "/api/capture/stop" ||
           path == "/api/capture/clear" ||
           path.startsWith("/api/marker/")) {
    if (!post) {
      send_method_not_allowed(client);
    } else if (!authorized_mutation) {
      send_forbidden(client);
    } else if (path == "/api/capture/start") {
      set_capture(true, false);
      send_json_ok(client);
    } else if (path == "/api/capture/stop") {
      set_capture(false, false);
      send_json_ok(client);
    } else if (path == "/api/capture/clear") {
      set_capture(false, true);
      send_json_ok(client);
    } else {
      const int state_separator = path.lastIndexOf('/');
      const String marker_text = path.substring(12, state_separator);
      const String state_text = path.substring(state_separator + 1);
      const bool valid_state = state_text == "on" || state_text == "off";
      const int marker_id = marker_text.toInt();
      const bool valid_id = marker_text.length() == 1 &&
                            marker_text[0] >= '0' && marker_text[0] <= '6' &&
                            marker_id < kMarkerCount;
      if (!valid_state || !valid_id)
        send_json_result(client, false, "marcador no válido");
      else if (!set_marker(uint8_t(marker_id), state_text == "on"))
        send_json_result(client, false,
                         "inicia la captura antes de marcar una alarma");
      else
        send_json_ok(client);
    }
  }
  else if (path.startsWith("/api/research/respond/") ||
           path.startsWith("/api/research/observe/")) {
    if (!post) {
      send_method_not_allowed(client);
    } else if (!authorized_mutation) {
      send_forbidden(client);
    } else if (path.startsWith("/api/research/respond/")) {
      const int id_separator = path.lastIndexOf('/');
      const String response = path.substring(22, id_separator);
      const uint32_t hypothesis_id =
          uint32_t(strtoul(path.substring(id_separator + 1).c_str(), nullptr, 10));
      if ((response != "yes" && response != "no") || hypothesis_id == 0)
        send_json_result(client, false, "respuesta no válida");
      else if (!respond_to_research_hypothesis(hypothesis_id,
                                                response == "yes"))
        send_json_result(client, false, "hipótesis caducada o ya respondida");
      else
        send_json_ok(client);
    } else {
      const int timestamp_separator = path.indexOf('/', 22);
      const String code_text = timestamp_separator < 0
                                   ? path.substring(22)
                                   : path.substring(22, timestamp_separator);
      const String timestamp_text = timestamp_separator < 0
                                        ? String()
                                        : path.substring(timestamp_separator + 1);
      const int code = code_text.toInt();
      const uint32_t event_uptime_ms = timestamp_text.isEmpty()
                                           ? 0
                                           : uint32_t(strtoul(
                                                 timestamp_text.c_str(),
                                                 nullptr, 10));
      bool valid_timestamp = true;
      for (size_t i = 0; i < timestamp_text.length(); i++)
        valid_timestamp &= isDigit(timestamp_text[i]);
      if (code_text.isEmpty() || code < 1 || code > kResearchCodeCount ||
          !valid_timestamp ||
          !add_research_observation(uint8_t(code), event_uptime_ms))
        send_json_result(client, false, "observación no válida");
      else
        send_json_ok(client);
    }
  }
  else send_not_found(client);
  client.flush();
  client.stop();
  if (reboot_after_response) {
    delay(350);
    ESP.restart();
  }
}

void DiagnosticWeb::send_page(WiFiClient& client) {
  response_headers(client, "200 OK", "text/html; charset=utf-8");
  client.print(FPSTR(kDashboard));
}

void DiagnosticWeb::send_research_page(WiFiClient& client) {
  response_headers(client, "200 OK", "text/html; charset=utf-8");
  client.print(FPSTR(kResearchPage));
}

void DiagnosticWeb::send_session(WiFiClient& client) {
  JsonDocument d;
  d["token"] = csrf_token_;
  response_headers(client, "200 OK", "application/json; charset=utf-8");
  serializeJson(d, client);
}

void DiagnosticWeb::send_status(WiFiClient& client) {
  DiagnosticRuntimeState s;
  uint16_t rows;
  bool active;
  uint32_t started_ms;
  bool markers[kMarkerCount];
  xSemaphoreTake(mutex_, portMAX_DELAY);
  s = runtime_;
  rows = capture_count_;
  active = capture_enabled_;
  started_ms = capture_started_ms_;
  memcpy(markers, marker_states_, sizeof(markers));
  xSemaphoreGive(mutex_);
  JsonDocument d;
  d["mode"] = s.test_mode ? "TEST" : "LIVE";
  d["webCore"] = 0;
  d["uptimeMs"] = millis();
  d["heap"] = ESP.getFreeHeap();
  d["wifi"]["connected"] = WiFi.status() == WL_CONNECTED;
  d["wifi"]["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "AP de diagnóstico";
  d["wifi"]["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  d["wifi"]["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  d["signalk"]["status"] = s.signalk_status;
  d["signalk"]["connected"] = strcmp(s.signalk_status, "Conectado") == 0;
  d["signalk"]["server"] = String(s.signalk_server) + ":" + s.signalk_port;
  d["can"]["started"] = s.can_started;
  d["can"]["listenOnly"] = true;
  d["can"]["txCalls"] = 0;
  d["can"]["bitrate"] = s.can_bitrate_kbps;
  d["can"]["locked"] = s.can_locked;
  d["can"]["bitrateSaved"] = s.can_bitrate_saved;
  d["can"]["rawFrames"] = s.can_raw_frame_count;
  d["can"]["extendedFrames"] = s.can_extended_frame_count;
  d["can"]["standardFrames"] = s.can_standard_frame_count;
  d["can"]["source"] = s.engine_source == 0xff ? "auto" : String("0x") + String(s.engine_source, HEX);
  d["can"]["frames"] = s.frame_count;
  d["can"]["unknown"] = s.unknown_frame_count;
  d["can"]["latestPgn"] = s.latest_pgn;
  if (isnan(s.rpm)) d["engine"]["rpm"] = nullptr;
  else d["engine"]["rpm"] = s.rpm;
  if (isnan(s.coolant_c)) d["engine"]["coolantC"] = nullptr;
  else d["engine"]["coolantC"] = s.coolant_c;
  if (isnan(s.oil_kpa)) d["engine"]["oilKpa"] = nullptr;
  else d["engine"]["oilKpa"] = s.oil_kpa;
  float voltage = !isnan(s.alternator_v)
                      ? s.alternator_v
                      : (!isnan(s.mdi_supply_v) ? s.mdi_supply_v
                                                : s.battery_v);
  if (isnan(voltage)) d["engine"]["voltage"] = nullptr;
  else d["engine"]["voltage"] = voltage;
  if (isnan(s.mdi_supply_v)) d["engine"]["mdiSupplyVoltage"] = nullptr;
  else d["engine"]["mdiSupplyVoltage"] = s.mdi_supply_v;
  d["mdi"]["detected"] = s.mdi_detected;
  d["mdi"]["mappingVerified"] = s.mapping_verified;
  d["mdi"]["dm1Available"] = s.dm1_available;
  d["alarms"]["overTemperature"] = s.over_temperature;
  d["alarms"]["lowOil"] = s.low_oil;
  d["alarms"]["lowVoltage"] = s.low_voltage;
  d["alarms"]["engineCheck"] = s.engine_check;
  d["alarms"]["dataFresh"] = s.alarm_data_fresh;
  d["manualRules"]["oil"]["valid"] = s.rule_oil_valid;
  d["manualRules"]["oil"]["condition"] = s.rule_oil_condition;
  d["manualRules"]["oil"]["on"] = s.rule_oil_on;
  d["manualRules"]["oil"]["elapsedMs"] = s.rule_oil_elapsed_ms;
  d["manualRules"]["oil"]["thresholdMs"] = s.rule_oil_threshold_ms;
  d["manualRules"]["oil"]["changedMs"] = s.rule_oil_changed_ms;
  d["manualRules"]["temperature"]["valid"] = s.rule_temp_valid;
  d["manualRules"]["temperature"]["condition"] = s.rule_temp_condition;
  d["manualRules"]["temperature"]["on"] = s.rule_temp_on;
  d["manualRules"]["temperature"]["elapsedMs"] = s.rule_temp_elapsed_ms;
  d["manualRules"]["temperature"]["thresholdMs"] = 15000;
  d["manualRules"]["temperature"]["changedMs"] = s.rule_temp_changed_ms;
  d["manualRules"]["lowVoltage"]["valid"] = s.rule_low_voltage_valid;
  d["manualRules"]["lowVoltage"]["condition"] =
      s.rule_low_voltage_condition;
  d["manualRules"]["lowVoltage"]["on"] = s.rule_low_voltage_on;
  d["manualRules"]["lowVoltage"]["elapsedMs"] =
      s.rule_low_voltage_elapsed_ms;
  d["manualRules"]["lowVoltage"]["thresholdMs"] = 10000;
  d["manualRules"]["lowVoltage"]["changedMs"] =
      s.rule_low_voltage_changed_ms;
  d["manualRules"]["highVoltage"]["valid"] = s.rule_high_voltage_valid;
  d["manualRules"]["highVoltage"]["condition"] =
      s.rule_high_voltage_condition;
  d["manualRules"]["highVoltage"]["on"] = s.rule_high_voltage_on;
  d["manualRules"]["highVoltage"]["elapsedMs"] =
      s.rule_high_voltage_elapsed_ms;
  d["manualRules"]["highVoltage"]["thresholdMs"] = 30000;
  d["manualRules"]["highVoltage"]["changedMs"] =
      s.rule_high_voltage_changed_ms;
  d["manualRules"]["voltageSource"] = "entrada MDI / SPN 158";
  d["capture"]["active"] = active;
  d["capture"]["rows"] = rows;
  d["capture"]["elapsedMs"] = active ? millis() - started_ms : 0;
  d["autoCapture"]["ready"] = persistent_ready_;
  d["autoCapture"]["rows"] = persistent_rows_;
  d["autoCapture"]["bytes"] = persistent_bytes_;
  d["autoCapture"]["dropped"] = persistent_dropped_;
  d["autoCapture"]["full"] = persistent_full_;
  {
    const size_t total = SPIFFS.totalBytes();
    const size_t used = SPIFFS.usedBytes();
    d["autoCapture"]["freeBytes"] = total > used ? total - used : 0;
  }
  JsonArray marker_array = d["capture"]["markers"].to<JsonArray>();
  for (bool marker : markers) marker_array.add(marker);
  String body; serializeJson(d, body);
  response_headers(client, "200 OK", "application/json; charset=utf-8"); client.print(body);
}

void DiagnosticWeb::send_research_status(WiFiClient& client) {
  ResearchItem history[kResearchHistorySlots];
  uint8_t head;
  uint8_t count;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  memcpy(history, research_history_, sizeof(history));
  head = research_history_head_;
  count = research_history_count_;
  xSemaphoreGive(mutex_);

  JsonDocument d;
  // Used by the browser to align performance.now() with the same millis()
  // clock that timestamps CAN frames. Multiple samples remove most RTT bias.
  d["serverUptimeMs"] = millis();
  if (count > 0) {
    const ResearchItem& latest =
        history[(head + kResearchHistorySlots - 1) % kResearchHistorySlots];
    d["id"] = latest.hypothesis_id;
    d["uptimeMs"] = latest.uptime_ms;
    d["pgn"] = latest.pgn;
    d["label"] = research_label(latest.code);
    d["confidence"] = research_confidence(latest.code);
    d["data"] = hex_bytes(latest.data, latest.len);
    d["pending"] = latest.response == 0;
    d["response"] = latest.response == 1 ? "CONFIRMADA" :
                    latest.response == 2 ? "RECHAZADA" : "pendiente";
  }
  d["rows"] = research_rows_;
  d["bytes"] = research_bytes_;
  JsonArray events = d["events"].to<JsonArray>();
  for (uint8_t n = 0; n < count; n++) {
    const uint8_t index =
        (head + kResearchHistorySlots - 1 - n) % kResearchHistorySlots;
    const ResearchItem& item = history[index];
    JsonObject event = events.add<JsonObject>();
    event["id"] = item.hypothesis_id;
    event["uptimeMs"] = item.uptime_ms;
    event["pgn"] = item.pgn;
    event["label"] = research_label(item.code);
    event["confidence"] = research_confidence(item.code);
    event["data"] = hex_bytes(item.data, item.len);
    event["response"] = item.response == 1 ? "CONFIRMADA" :
                        item.response == 2 ? "RECHAZADA" : "pendiente";
    event["pending"] = item.response == 0;
  }
  String body;
  serializeJson(d, body);
  response_headers(client, "200 OK", "application/json; charset=utf-8");
  client.print(body);
}

void DiagnosticWeb::send_frames(WiFiClient& client) {
  FrameSlot local[kFrameSlots];
  xSemaphoreTake(mutex_, portMAX_DELAY); memcpy(local, frames_, sizeof(local)); xSemaphoreGive(mutex_);
  JsonDocument d;
  JsonArray items = d["frames"].to<JsonArray>();
  const uint32_t now = millis();
  for (const auto& f : local) {
    if (!f.used) continue;
    JsonObject o = items.add<JsonObject>();
    o["pgn"] = f.pgn;
    char hex[9]; snprintf(hex, sizeof(hex), "%05lX", (unsigned long)f.pgn); o["pgnHex"] = hex;
    o["name"] = pgn_name(f.pgn);
    char source[3]; snprintf(source, sizeof(source), "%02X", f.source); o["source"] = source;
    const uint32_t span = max<uint32_t>(1, f.last_ms - f.first_ms);
    o["hz"] = f.count < 2 ? 0 : roundf((float(f.count - 1) * 10000.f) / span) / 10.f;
    o["data"] = hex_bytes(f.data, f.len);
    o["conversion"] = conversion_text(f.pgn, f.data, f.len);
    o["ageMs"] = now - f.last_ms;
  }
  String body; serializeJson(d, body);
  response_headers(client, "200 OK", "application/json; charset=utf-8"); client.print(body);
}

void DiagnosticWeb::send_capture(WiFiClient& client) {
  CaptureRow local[kCaptureSlots];
  uint16_t count, head;
  xSemaphoreTake(mutex_, portMAX_DELAY); memcpy(local, capture_, sizeof(local)); count = capture_count_; head = capture_head_; xSemaphoreGive(mutex_);
  response_headers(client, "200 OK", "text/csv; charset=utf-8");
  client.println("session,relative_ms,uptime_ms,type,event,state,can_id_hex,pgn,pgn_hex,source_hex,name,length,data_hex,conversion");
  uint16_t start = (head + kCaptureSlots - count) % kCaptureSlots;
  for (uint16_t n = 0; n < count && client.connected(); n++) {
    const CaptureRow& r = local[(start + n) % kCaptureSlots];
    if (r.kind == 1) {
      client.printf("%u,%lu,%lu,marker,%s,%s,,,,,,,,\r\n", r.session,
                    (unsigned long)r.relative_ms,
                    (unsigned long)r.timestamp_ms, marker_name(r.marker_id),
                    r.marker_active ? "ON" : "OFF");
      continue;
    }
    String bytes = hex_bytes(r.data, r.len);
    String conversion = conversion_text(r.pgn, r.data, r.len); conversion.replace('"', '\'');
    client.printf("%u,%lu,%lu,frame,,,0x%08lX,%lu,0x%05lX,0x%02X,\"%s\",%u,\"%s\",\"%s\"\r\n",
                  r.session, (unsigned long)r.relative_ms,
                  (unsigned long)r.timestamp_ms, (unsigned long)r.can_id,
                  (unsigned long)r.pgn, (unsigned long)r.pgn, r.source,
                  pgn_name(r.pgn), r.len, bytes.c_str(), conversion.c_str());
    if ((n & 7) == 7) vTaskDelay(1);
  }
}

void DiagnosticWeb::send_persistent_capture(WiFiClient& client,
                                             bool previous) {
  const char* capture_path =
      previous ? kPreviousCapturePath : kPersistentCapturePath;
  if (!persistent_ready_ || !SPIFFS.exists(capture_path)) {
    send_not_found(client);
    return;
  }
  File file = SPIFFS.open(capture_path, FILE_READ);
  if (!file) {
    send_not_found(client);
    return;
  }
  response_headers(client, "200 OK", "text/csv; charset=utf-8");
  uint8_t buffer[768];
  while (file.available() && client.connected()) {
    const size_t count = file.read(buffer, sizeof(buffer));
    if (count == 0) break;
    client.write(buffer, count);
    vTaskDelay(1);
  }
  file.close();
}

void DiagnosticWeb::send_research_capture(WiFiClient& client) {
  if (!SPIFFS.exists(kResearchCapturePath)) {
    send_not_found(client);
    return;
  }
  File file = SPIFFS.open(kResearchCapturePath, FILE_READ);
  if (!file) {
    send_not_found(client);
    return;
  }
  response_headers(client, "200 OK", "text/csv; charset=utf-8");
  uint8_t buffer[512];
  while (file.available() && client.connected()) {
    const size_t count = file.read(buffer, sizeof(buffer));
    if (count == 0) break;
    client.write(buffer, count);
    vTaskDelay(1);
  }
  file.close();
}

bool DiagnosticWeb::receive_firmware(WiFiClient& client,
                                     size_t content_length) {
  // Update.begin selects the inactive OTA partition and rejects images that do
  // not fit. The running image is not overwritten.
  if (!Update.begin(content_length, U_FLASH)) {
    String error = Update.errorString();
    send_json_result(client, false, error.c_str());
    return false;
  }

  uint8_t buffer[1024];
  size_t remaining = content_length;
  uint32_t deadline = millis() + 10000;
  while (remaining > 0) {
    if (!client.available()) {
      if (!client.connected() || int32_t(millis() - deadline) >= 0) {
        Update.abort();
        send_json_result(client, false, "tiempo de espera agotado");
        return false;
      }
      vTaskDelay(1);
      continue;
    }
    const size_t requested = min<size_t>(sizeof(buffer), remaining);
    const int received = client.read(buffer, requested);
    if (received <= 0) continue;
    if (Update.write(buffer, size_t(received)) != size_t(received)) {
      String error = Update.errorString();
      Update.abort();
      send_json_result(client, false, error.c_str());
      return false;
    }
    remaining -= size_t(received);
    deadline = millis() + 10000;
    vTaskDelay(1);
  }

  if (!Update.end(true) || !Update.isFinished()) {
    String error = Update.errorString();
    send_json_result(client, false, error.c_str());
    return false;
  }
  send_json_ok(client);
  return true;
}

void DiagnosticWeb::begin_persistent_capture() {
  // SensESP owns and mounts this SPIFFS partition. Never auto-format it here:
  // doing so could erase the saved WiFi and Signal K configuration.
  if (!SPIFFS.begin(false)) {
    Serial.println("Automatic capture disabled: SPIFFS mount failed");
    return;
  }
  File file = SPIFFS.open(kPersistentCapturePath, FILE_APPEND);
  if (!file) {
    Serial.println("Automatic capture disabled: cannot open CSV");
    return;
  }
  if (file.size() == 0) file.print(kPersistentCaptureHeader);
  persistent_bytes_ = file.size();

  struct timeval tv = {};
  gettimeofday(&tv, nullptr);
  char utc[24] = {};
  if (tv.tv_sec >= 1609459200) {
    struct tm when = {};
    gmtime_r(&tv.tv_sec, &when);
    strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &when);
  }
  file.printf("%08lX,%s,%lld,%lu,boot,,,,,0,0x00,0x00,,\r\n",
              (unsigned long)persistent_boot_id_, utc,
              tv.tv_sec >= 1609459200
                  ? (static_cast<long long>(tv.tv_sec) * 1000LL +
                     tv.tv_usec / 1000)
                  : 0LL,
              (unsigned long)millis());
  file.flush();
  persistent_bytes_ = file.size();
  file.close();
  persistent_ready_ = true;
  if (SPIFFS.exists(kResearchCapturePath)) {
    File research_file = SPIFFS.open(kResearchCapturePath, FILE_READ);
    if (research_file) {
      research_bytes_ = research_file.size();
      research_file.close();
    }
  }
  Serial.printf("Automatic change capture: %s (%u KiB rotation)\n",
                kPersistentCapturePath,
                unsigned(kPersistentCaptureMaxBytes / 1024U));
}

void DiagnosticWeb::drain_persistent_queue() {
  if (!persistent_ready_ || persistent_queue_ == nullptr) return;
  PersistentRow row;
  uint8_t drained = 0;
  while (drained < 12 && xQueueReceive(persistent_queue_, &row, 0) == pdTRUE) {
    append_persistent_row(row);
    drained++;
  }
}

bool DiagnosticWeb::clear_persistent_capture() {
  if (!persistent_ready_) return false;
  // This endpoint deliberately touches only the two automatic capture files.
  // SensESP configuration, WiFi credentials and research_events.csv remain.
  if (persistent_queue_ != nullptr) xQueueReset(persistent_queue_);
  SPIFFS.remove(kPreviousCapturePath);
  SPIFFS.remove(kPersistentCapturePath);
  File file = SPIFFS.open(kPersistentCapturePath, FILE_WRITE);
  if (!file) return false;
  file.print(kPersistentCaptureHeader);

  struct timeval tv = {};
  gettimeofday(&tv, nullptr);
  char utc[24] = {};
  long long epoch_ms = 0;
  if (tv.tv_sec >= 1609459200) {
    struct tm when = {};
    gmtime_r(&tv.tv_sec, &when);
    strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &when);
    epoch_ms = static_cast<long long>(tv.tv_sec) * 1000LL + tv.tv_usec / 1000;
  }
  file.printf("%08lX,%s,%lld,%lu,clear,,,,,0,0x00,0x00,,\r\n",
              (unsigned long)persistent_boot_id_, utc, epoch_ms,
              (unsigned long)millis());
  file.flush();
  persistent_bytes_ = file.size();
  file.close();
  persistent_rows_ = 0;
  persistent_dropped_ = 0;
  persistent_full_ = false;
  Serial.println("Automatic capture files cleared; configuration preserved");
  return true;
}

// SPIFFS reports a short write instead of failing, so an append that silently
// wrote nothing used to leave persistent_bytes_ frozen below the rotation
// threshold. Rotation then never ran and the filesystem stayed full forever,
// which is what stopped the SensESP WiFi configuration from being saved.
bool DiagnosticWeb::persistent_space_available() {
  const size_t total = SPIFFS.totalBytes();
  const size_t used = SPIFFS.usedBytes();
  const size_t free_bytes = total > used ? total - used : 0;
  if (free_bytes < kPersistentReservedBytes) {
    if (!persistent_full_) {
      Serial.printf(
          "Automatic capture paused: only %u KiB free, %u KiB reserved for "
          "configuration\n",
          unsigned(free_bytes / 1024U), unsigned(kPersistentReservedBytes / 1024U));
    }
    persistent_full_ = true;
    return false;
  }
  if (persistent_full_) {
    Serial.println("Automatic capture resumed: filesystem space recovered");
    persistent_full_ = false;
  }
  return true;
}

void DiagnosticWeb::append_persistent_row(const PersistentRow& row) {
  rotate_persistent_capture_if_needed();
  if (!persistent_space_available()) {
    persistent_dropped_++;
    return;
  }
  File file = SPIFFS.open(kPersistentCapturePath, FILE_APPEND);
  if (!file) {
    persistent_dropped_++;
    return;
  }
  const size_t size_before = file.size();

  struct timeval tv = {};
  gettimeofday(&tv, nullptr);
  char utc[24] = {};
  long long epoch_ms = 0;
  if (tv.tv_sec >= 1609459200) {
    struct tm when = {};
    gmtime_r(&tv.tv_sec, &when);
    strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &when);
    epoch_ms = static_cast<long long>(tv.tv_sec) * 1000LL + tv.tv_usec / 1000;
  }
  String bytes = hex_bytes(row.data, row.len);
  String xor_bytes = hex_bytes(row.xor_data, row.len);
  const size_t written = file.printf(
      "%08lX,%s,%lld,%lu,%s,0x%08lX,%lu,0x%05lX,0x%02X,%u,0x%02X,0x%02X,"
      "\"%s\",\"%s\"\r\n",
      (unsigned long)persistent_boot_id_, utc, epoch_ms,
      (unsigned long)row.uptime_ms, row.baseline ? "baseline" : "change",
      (unsigned long)row.can_id, (unsigned long)row.pgn,
      (unsigned long)row.pgn, row.source, row.len, row.raw_changed_mask,
      row.trigger_mask, xor_bytes.c_str(), bytes.c_str());
  // Flush every relevant transition so an engine/ignition power-off does not
  // leave the final part of the shutdown sequence only in a RAM cache.
  file.flush();
  const size_t size_after = file.size();
  file.close();
  // A short write means the filesystem is out of usable space. Account for it
  // instead of looping forever on a file that can no longer grow.
  if (written == 0 || size_after <= size_before) {
    persistent_dropped_++;
    persistent_full_ = true;
    Serial.println("Automatic capture paused: flash write returned no bytes");
    return;
  }
  persistent_bytes_ = size_after;
  persistent_rows_++;
}

void DiagnosticWeb::append_research_row(const ResearchRow& row) {
  if (!persistent_ready_ || !persistent_space_available()) return;
  File file = SPIFFS.open(kResearchCapturePath, FILE_APPEND);
  if (!file) return;
  if (file.size() == 0) file.print(kResearchCaptureHeader);

  struct timeval tv = {};
  gettimeofday(&tv, nullptr);
  char utc[24] = {};
  long long epoch_ms = 0;
  if (tv.tv_sec >= 1609459200) {
    struct tm when = {};
    gmtime_r(&tv.tv_sec, &when);
    strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &when);
    epoch_ms = static_cast<long long>(tv.tv_sec) * 1000LL + tv.tv_usec / 1000;
  }
  const char* event = row.event == 1 ? "confirmation" :
                      row.event == 2 ? "observation" : "hypothesis";
  const char* response = row.response == 1 ? "yes" :
                         row.response == 2 ? "no" : "";
  const String bytes = hex_bytes(row.data, row.len);
  file.printf(
      "%08lX,%s,%lld,%lu,%s,%lu,%s,\"%s\",%u,%s,%lu,\"%s\"\r\n",
      (unsigned long)persistent_boot_id_, utc, epoch_ms,
      (unsigned long)row.uptime_ms, event,
      (unsigned long)row.hypothesis_id, research_code_name(row.code),
      research_label(row.code), research_confidence(row.code), response,
      (unsigned long)row.pgn, bytes.c_str());
  file.flush();
  research_bytes_ = file.size();
  file.close();
  research_rows_++;
}

void DiagnosticWeb::rotate_persistent_capture_if_needed() {
  // Running out of space before reaching the rotation threshold would deadlock
  // the capture: no rotation means nothing is ever reclaimed. Drop the older
  // generation first; the current one keeps the most recent evidence.
  if (persistent_full_ && persistent_bytes_ < kPersistentCaptureMaxBytes) {
    if (SPIFFS.exists(kPreviousCapturePath)) {
      SPIFFS.remove(kPreviousCapturePath);
      Serial.println(
          "Automatic capture: previous generation discarded to reclaim space");
    }
    return;
  }
  if (persistent_bytes_ < kPersistentCaptureMaxBytes) return;
  SPIFFS.remove(kPreviousCapturePath);
  if (!SPIFFS.rename(kPersistentCapturePath, kPreviousCapturePath)) {
    Serial.println("Automatic capture rotation failed; keeping current file");
    return;
  }
  File file = SPIFFS.open(kPersistentCapturePath, FILE_WRITE);
  if (!file) {
    persistent_ready_ = false;
    Serial.println("Automatic capture disabled after rotation open failure");
    return;
  }
  file.print(kPersistentCaptureHeader);
  file.flush();
  persistent_bytes_ = file.size();
  file.close();
}

void DiagnosticWeb::set_capture(bool enabled, bool clear) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (enabled && !capture_enabled_) {
    capture_started_ms_ = millis();
    capture_session_++;
    memset(marker_states_, 0, sizeof(marker_states_));
  }
  capture_enabled_ = enabled;
  if (clear) {
    capture_head_ = 0;
    capture_count_ = 0;
    capture_started_ms_ = 0;
    capture_session_ = 0;
    memset(capture_, 0, sizeof(capture_));
    memset(marker_states_, 0, sizeof(marker_states_));
  }
  xSemaphoreGive(mutex_);
}

void DiagnosticWeb::append_capture_row_locked(const CaptureRow& row) {
  capture_[capture_head_] = row;
  capture_head_ = (capture_head_ + 1) % kCaptureSlots;
  if (capture_count_ < kCaptureSlots) capture_count_++;
}

bool DiagnosticWeb::set_marker(uint8_t marker_id, bool active) {
  if (marker_id >= kMarkerCount) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (!capture_enabled_) {
    xSemaphoreGive(mutex_);
    return false;
  }
  if (marker_states_[marker_id] != active) {
    marker_states_[marker_id] = active;
    CaptureRow row;
    row.kind = 1;
    row.marker_id = marker_id;
    row.marker_active = active;
    row.timestamp_ms = millis();
    row.relative_ms = row.timestamp_ms - capture_started_ms_;
    row.session = capture_session_;
    append_capture_row_locked(row);
  }
  xSemaphoreGive(mutex_);
  return true;
}

void DiagnosticWeb::send_json_ok(WiFiClient& client) {
  static constexpr char body[] = "{\"ok\":true}";
  client.printf(
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
      "Content-Length: %u\r\nCache-Control: no-store\r\n"
      "Connection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n%s",
      unsigned(sizeof(body) - 1), body);
}
void DiagnosticWeb::send_json_result(WiFiClient& client, bool ok, const char* error) {
  response_headers(client, ok ? "200 OK" : "409 Conflict", "application/json; charset=utf-8");
  JsonDocument d;
  d["ok"] = ok;
  if (error != nullptr) d["error"] = error;
  serializeJson(d, client);
}
void DiagnosticWeb::send_forbidden(WiFiClient& client) {
  response_headers(client, "403 Forbidden", "application/json");
  client.print("{\"error\":\"forbidden\"}");
}
void DiagnosticWeb::send_method_not_allowed(WiFiClient& client) {
  response_headers(client, "405 Method Not Allowed", "application/json");
  client.print("{\"error\":\"method not allowed\"}");
}
void DiagnosticWeb::send_not_found(WiFiClient& client) { response_headers(client, "404 Not Found", "application/json"); client.print("{\"error\":\"not found\"}"); }
