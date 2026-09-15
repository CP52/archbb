#!/usr/bin/env node
/* ============================================================================
 *  ArchBB — prova_app.js
 *  Carica l'app in un DOM headless, le passa una sessione VERA e verifica che
 *  le schermate si popolino davvero.
 * ----------------------------------------------------------------------------
 *  PERCHE' ESISTE
 *    Il controllo `node --check` verifica la sintassi, non il comportamento.
 *    Un `getElementById('mFermR')` che ritorna null passa il controllo di
 *    sintassi benissimo, poi in esecuzione lancia e uccide l'intero
 *    renderDash: la dashboard resta vuota senza dire perche'. E' successo, e
 *    l'ho scoperto solo perche' l'ha visto l'utente.
 *
 *    Qui l'app viene ESEGUITA: si simula la connessione, si inietta una
 *    sessione letta da disco, si chiamano le funzioni di rendering e si
 *    controlla che le caselle contengano qualcosa. Un'eccezione in un
 *    qualunque render fa fallire la prova.
 *
 *  USO   node tools/prova_app.js <app.html> <cartella_SESS_...>
 * ========================================================================== */
const fs = require('fs');
const path = require('path');
const { JSDOM } = require('jsdom');

const appPath = process.argv[2];
const sessDir = process.argv[3];
if (!appPath || !sessDir) {
  console.error('uso: node prova_app.js <app.html> <cartella_SESS_...>');
  process.exit(2);
}

const html = fs.readFileSync(appPath, 'utf8');
const dom = new JSDOM(html, { runScripts: 'dangerously', pretendToBeVisual: true });
const w = dom.window;

// Canvas non c'e' in jsdom: uno stub inerte basta, i grafici non sono
// l'oggetto di questa prova (per quelli servirebbe un raster da confrontare).
w.HTMLCanvasElement.prototype.getContext = () => new Proxy({}, {
  get: (t, k) => (k === 'canvas' ? {} : () => {}),
  set: () => true,
});
w.indexedDB = undefined;   // niente persistenza nella prova

let errori = [];
w.addEventListener('error', e => errori.push('window.onerror: ' + e.message));
const warn = w.console.warn;
w.console.warn = (...a) => { if (String(a[0]).includes('ArchBB:')) errori.push(a.join(' ')); warn(...a); };

function ok(cond, msg) {
  console.log((cond ? '  ok      ' : '  FALLITO ') + msg);
  if (!cond) process.exitCode = 1;
}

setTimeout(() => {
  // Le `const`/`let` di primo livello di uno script classico NON diventano
  // proprieta' di window: vivono nell'ambiente lessicale globale. Si
  // raggiungono con eval nel contesto della pagina, non con w.NOME.
  const ev = expr => w.eval(expr);
  let D;
  try { D = ev('DB'); } catch (e) { }
  if (!D) { console.error("DB non raggiungibile: l'app non si e' inizializzata"); process.exit(1); }

  // --- carico una sessione vera dal disco ---------------------------------
  const meta = ev('parseSessionTxt')(fs.readFileSync(path.join(sessDir, 'session.txt'), 'utf8'));
  const { shots } = ev('parseShotsCsv')(fs.readFileSync(path.join(sessDir, 'shots.csv'), 'utf8'));
  const nome = path.basename(sessDir);
  D.sessions[nome] = { nome, name: nome, meta, shots, source: 'sd', csvComment: '' };
  D.current = nome;

  let onde = 0;
  for (const f of fs.readdirSync(sessDir).filter(x => /^burst_\d+\.bin$/i.test(x))) {
    const nb = fs.readFileSync(path.join(sessDir, f));
    const ab = nb.buffer.slice(nb.byteOffset, nb.byteOffset + nb.byteLength);
    const b = ev('parseBurst')(ab);
    const x = shots.find(o => o.burstFile === f);
    if (b && x) { x.burst = b; onde++; }
  }
  console.log(`\nSessione: ${nome} — ${shots.length} tiri, ${onde} onde\n`);

  // --- rendering di ogni scheda ------------------------------------------
  for (const tab of ['dash', 'tiri', 'onda', 'fft', 'traccia']) {
    try { ev('showTab')(tab); ok(true, 'scheda ' + tab.toUpperCase() + ' disegnata senza eccezioni'); }
    catch (e) { ok(false, 'scheda ' + tab.toUpperCase() + ' -> ' + e.message); }
  }

  // --- il QUADRO deve essere POPOLATO, non solo non esplodere -------------
  console.log('');
  ev('showTab')('dash');
  const pillole = ['mFerm', 'mCant', 'mXi', 'mPul', 'mNet', 'mTor', 'mHold', 'mFT'];
  for (const id of pillole) {
    const e = w.document.getElementById(id);
    const r = w.document.getElementById(id + 'R');
    ok(!!e, 'pillola ' + id + ' presente');
    ok(!!r, 'riferimento ' + id + 'R presente');
    if (e) ok(e.textContent.trim().length > 0, id + ' valorizzata: "' + e.textContent.trim() + '"');
  }

  // Le quattro che devono avere un NUMERO su questi dati.
  console.log('');
  for (const id of ['mFerm', 'mCant', 'mPul', 'mTor', 'mHold']) {
    const e = w.document.getElementById(id);
    ok(e && /[0-9]/.test(e.textContent), id + ' contiene un numero: "' + e.textContent.trim() + '"');
  }

  // --- navigazione fra i tiri --------------------------------------------
  console.log('');
  const prima = w.document.getElementById('mFerm').textContent;
  ev('stepShot')(1);
  const dopo = w.document.getElementById('mFerm').textContent;
  ok(ev('selShot') === 1, 'stepShot(+1) muove la selezione');
  ok(prima !== dopo, 'i valori cambiano cambiando tiro (' + prima.trim() + ' -> ' + dopo.trim() + ')');
  const pos = w.document.getElementById('navPos').textContent;
  ok(/2 di /.test(pos), 'la posizione si aggiorna: "' + pos + '"');
  ev('stepShot')(-1);
  ok(ev('selShot') === 0, 'stepShot(-1) torna indietro');
  ok(w.document.getElementById('navPrev').disabled, 'freccia indietro disabilitata al primo tiro');

  // --- coerenza fra chip e asse del grafico -------------------------------
  console.log('');
  ev('showTab')('tiri');
  // I NOMI DEVONO ESSERE QUELLI FISICI, sempre: mai una formula al posto di
  // "alzo" o "cant". Ordine richiesto: alzo per primo, cant per secondo.
  const chips = [...w.document.querySelectorAll('#trendChips .chip')].map(x => x.textContent.trim());
  ok(chips[0] === 'alzo' && chips[1] === 'cant',
     'chip di andamento: ' + chips.join(' | '));
  ok(!chips.some(c => /atan2|ay\/ax|az\/ax/.test(c)), 'nessuna formula nelle chip');

  const th = [...w.document.querySelectorAll('#tblShots th')].map(x => x.textContent);
  ok(th[2] === 'alzo°' && th[3] === 'cant°',
     'intestazioni tabella: ' + th.slice(0, 5).join(' | '));
  ok(!th.some(t => /atan2/.test(t)), 'nessuna formula nelle intestazioni');

  // L'asse Y del grafico deve dire la stessa cosa della chip selezionata.
  const labs = [];
  const origPlot = ev('plot');
  w.eval('window.__labs = [];');
  w.plot = (cv, cfg) => { w.__labs.push(cfg.yLabel); return origPlot(cv, cfg); };
  w.eval("trendMetric='alzo'; drawTrend(DB.sessions[DB.current].shots);");
  w.eval("trendMetric='cant'; drawTrend(DB.sessions[DB.current].shots);");
  const yl = w.__labs.slice(-2);
  ok(yl[0] === 'alzo [°]' && yl[1] === 'cant [°]', 'asse Y: ' + yl.join(' | '));

  // La seconda metrica della fase MIRA e' l'ANGOLO DI MIRA, non il cant.
  const kMira = [...w.document.querySelectorAll('.fase1 .pill .k')].map(x => x.textContent.trim());
  ok(kMira[1] === 'angolo di mira', 'fase MIRA, 2a metrica: "' + kMira[1] + '"');
  ok(!kMira.some(k => /cant/i.test(k)), 'nessun "cant" nella fase MIRA: ' + kMira.join(' | '));

  console.log('');
  if (errori.length) { console.log('ELEMENTI ASSENTI SEGNALATI:'); errori.forEach(e => console.log('  ' + e)); process.exitCode = 1; }
  else console.log('nessun elemento DOM mancante');
}, 400);
