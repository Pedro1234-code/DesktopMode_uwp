(function () {
 'use strict';
 var term = new Terminal({fontFamily:'Consolas, monospace',fontSize:16,scrollback:5000,convertEol:false,theme:{background:'#0c0c0c'}});
 var fit = new FitAddon.FitAddon(); term.loadAddon(fit); term.open(document.getElementById('terminal'));
 var mode = 7, line = '', enabled = true;
 function notify(value) { window.external.notify(JSON.stringify(value)); }
 function echo(value) { if (mode & 4) term.write(value); }
 term.onData(function (data) {
  if (!enabled) return;
  if (!(mode & 2)) { notify({input:data}); return; }
  // Cooked input is retained until Enter, as required by ENABLE_LINE_INPUT.
  data=data.replace(/\r\n/g,'\r').replace(/\x1b\[[0-?]*[ -/]*[@-~]/g,'');
  for (var ch of data) {
   if (ch === '\x03' && (mode & 1)) { line=''; notify({input:ch}); }
   else if (ch === '\r' || ch === '\n') { echo('\r\n'); notify({input:line+'\r\n'}); line=''; }
   else if (ch === '\x7f' || ch === '\b') { if(line.length) { line=Array.from(line).slice(0,-1).join(''); echo('\b \b'); } }
   else if (ch >= ' ' && ch !== '\x7f') { line+=ch; echo(ch); }
  }
 });
 window.bridgeOutput=function(text){term.write(text);};
 window.bridgeMode=function(value){mode=value;};
 window.bridgeEnabled=function(value){enabled=value===true || value==='true'; term.options.disableStdin=!enabled;};
 window.bridgeFit=function(){fit.fit();notify({cols:term.cols,rows:term.rows});};
 window.addEventListener('resize',window.bridgeFit);
 window.bridgeFit(); term.focus(); notify({ready:true});
}());
