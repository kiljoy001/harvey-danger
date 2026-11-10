import { chromium } from '../../../halatha/node_modules/playwright/index.mjs';
import { writeFileSync } from 'node:fs';

// Requires a built Halatha guest and its static server.
const url = process.argv[2] || 'http://127.0.0.1:7072/examples/basic-v64-aspera.html?test';
const browser = await chromium.launch({ args: ['--no-sandbox'] });
const page = await browser.newPage({ viewport: { width: 1100, height: 1000 } });
const errors = [];
page.on('pageerror', e => errors.push(e.message));
const pause = ms => page.waitForTimeout(ms);
const grab = async name => {
  const data = await page.locator('#vga canvas').evaluate(c => c.toDataURL());
  writeFileSync(`/tmp/rio-titlebars-${name}.png`, Buffer.from(data.split(',')[1], 'base64'));
};
const text = async value => {
  for (const ch of value) {
    if (ch === '>') await page.keyboard.press('Shift+.');
    else await page.keyboard.press(ch);
    await pause(45);
  }
};
const command = async value => {
  await page.keyboard.press('Control+u');
  await text(value);
  await page.keyboard.press('Enter');
  await pause(1800);
};
const hostText = path => page.evaluate(path => {
  const data = window.aspera.fs.lookup(path)?.data;
  return data ? new TextDecoder().decode(data) : null;
}, path);
const waitFile = (path, content) => page.waitForFunction(([path, content]) => {
  const data = window.aspera.fs.lookup(path)?.data;
  return data && new TextDecoder().decode(data) === content;
}, [path, content], { timeout: 30000 });
const move = (dx, dy) => page.evaluate(([dx, dy]) => {
  document.querySelector('#vga canvas').dispatchEvent(new MouseEvent('mousemove', { movementX: dx, movementY: dy, bubbles: true }));
}, [dx, dy]);
const point = async (x, y) => {
  for (let i = 0; i < 20; i++) await move(-127, -127);
  await pause(300);
  while (x > 0) { const d = Math.min(x, 100); await move(d, 0); x -= d; await pause(30); }
  while (y > 0) { const d = Math.min(y, 100); await move(0, d); y -= d; await pause(30); }
  await pause(300);
};
const buttons = left => page.evaluate(left => window.emulator.bus.send('mouse-click', [left, false, false]), left);
const q = s => "'" + s.replaceAll("'", "''") + "'";
const put = (path,data) => page.evaluate(([path,data]) => {
  const fs=window.aspera.fs, node=fs.lookup(path);
  if(node){node.data=new TextEncoder().encode(data);node.version++;}
  else fs.addFile(path,data,{writable:true,mode:0o600});
},[path,data]);
let request=0;
const run = async script => {
  const token='request'+(++request);
  await put('clip/request',script+'\necho '+token+' > /n/host/clip/token\n');
  await waitFile('clip/ack',token+'\n');
};
const ids = async () => {
  await run('ls /dev/wsys > /n/host/clip/ids');
  return (await hostText('clip/ids')).trim().split(/\s+/).map(s=>Number(s.split('/').pop()));
};
const body = async id => {
  await run(`cat /dev/wsys/${id}/text > /n/host/clip/seen`);
  return hostText('clip/seen');
};
const opened = async (path, before) => {
  if(!before){before=await ids();await run('plumb '+q(path));}
  for(let n=0;n<12;n++){
    const added=(await ids()).filter(id=>!before.includes(id));
    if(added.length) return added[0];
    await pause(500);
  }
  throw Error('No window for '+path);
};
const close = id => run(`echo 'delete -id ${id}' > /dev/wctl`);
try {
  await page.goto(url);
  await page.waitForFunction(() => document.querySelector('#serial').textContent.includes('halatha: starting Rio'), null, {timeout:120000});
  await pause(10000);
  const unicode='a'.repeat(4095)+'€λ🙂\n'+'b'.repeat(32760)+'終わり\n';
  const large=('Text line: 0123456789 abcdefghijklmnopqrstuvwxyz\n').repeat(24000);
  await put("clip/boundary's name.txt",unicode);
  await put('clip/large.txt',large);
  await put('clip/empty.txt','');
  await put('clip/small.txt','still working\n');
  await put('clip/slow.txt','delayed text\n');
  await put('clip/driver',"while(){\n if(test -e /n/host/clip/request){\n rc /n/host/clip/request\n rm /n/host/clip/request\n cat /n/host/clip/token > /n/host/clip/ack\n }\n sleep 1\n}\n");
  await command('rc /n/host/clip/driver');
  let id=await opened("/n/host/clip/boundary's name.txt");
  if(await body(id)!==unicode) throw Error('UTF-8 changed while opening');
  await page.keyboard.press('Control+s'); await pause(500);
  if(await hostText("clip/boundary's name.txt")!==unicode) throw Error('UTF-8 changed on save');
  await close(id);
  console.log('Quoted path, UTF-8 read boundaries, byte-exact save: PASS');
  const start=Date.now();
  id=await opened('/n/host/clip/large.txt');
  if(await body(id)!==large) throw Error('Large text lost data');
  await page.keyboard.press('Control+s'); await pause(500);
  if(await hostText('clip/large.txt')!==large) throw Error('Large file changed on save');
  console.log('Large text (over 1 MiB), exact read/save: PASS',Date.now()-start,'ms including driver polling');
  await close(id);
  id=await opened('/n/host/clip/empty.txt');
  if(await body(id)!=='') throw Error('Empty file contains a shell prompt');
  await close(id);
  for(let n=0;n<3;n++){
    id=await opened('/n/host/clip/small.txt');
    if(await body(id)!=='still working\n') throw Error('Repeated open failed');
    await close(id);
  }
  console.log('Empty and repeated text opens: PASS');
  const first=await opened('/n/host/clip/small.txt');
  const second=await opened('/n/host/clip/small.txt');
  await close(first);
  await run(`echo 'current -id ${second}' > /dev/wctl`);
  await page.keyboard.press('Control+End');
  await text('edited');await page.keyboard.press('Enter');await page.keyboard.press('Control+s');
  await waitFile('clip/small.txt','still working\nedited\n');
  await close(second);
  await put('clip/small.txt','still working\n');
  console.log('Two windows on the same file have independent undo ownership: PASS');

  // Binary data after the old 4096-byte probe must never enter an editable buffer.
  await page.evaluate(() => {
    const data=new Uint8Array(5002).fill(65);data[5000]=0;
    window.aspera.fs.addFile('clip/binary.dat',data,{writable:true,mode:0o600});
  });
  id=await opened('/n/host/clip/binary.dat');
  await pause(1000);
  if(!(await body(id)).includes('Read-only')) throw Error('Binary file has no visible preview');
  await close(id);
  id=await opened('/n/host/clip/missing.txt');
  if(!(await body(id)).match(/no such|not exist|not found/)) throw Error('Missing file error invisible');
  await close(id);
  console.log('Binary fallback and missing-file feedback: PASS');
  await page.evaluate(()=>{
    window.aspera.fs.addFile('clip/read-error.txt','partial data',{read:(offset,count)=>{
      if(offset)throw Error('injected read failure');
      return new TextEncoder().encode('part');
    }});
    window.aspera.fs.addFile('clip/oversized.txt',new Uint8Array(4*1024*1024+1).fill(65));
  });
  id=await opened('/n/host/clip/read-error.txt');
  if(!(await body(id)).includes('injected read failure'))throw Error('Partial read silently became an editable file');
  await close(id);
  id=await opened('/n/host/clip/oversized.txt');
  if(!(await body(id)).includes('Read-only'))throw Error('Oversized file did not get a bounded preview');
  await close(id);
  console.log('Partial-read failure and oversized-file fallback: PASS');

  // Withhold one 9P read, but honor its eventual flush. All other files work.
  await page.evaluate(() => {
    const fs=window.aspera.fs, original=fs.handle.bind(fs);
    window.slowReads=0;window.slowFlushes=0;
    const pending=new Map();
    window.releaseSlow=()=>{for(const complete of pending.values())complete();pending.clear();};
    fs.handle=(req,reply)=>{
      const d=new DataView(req.buffer,req.byteOffset,req.byteLength);
      if(req[4]===116 && fs.fids.get(d.getUint32(7,true))?.node.name==='slow.txt'){
        const saved=req.slice();pending.set(d.getUint16(5,true),()=>original(saved,reply));window.slowReads++;return;
      }
      if(req[4]===108 && pending.delete(d.getUint16(7,true))) window.slowFlushes++;
      return original(req,reply);
    };
  });
  const before=await ids();
  await run('plumb /n/host/clip/slow.txt\nplumb /n/host/clip/small.txt');
  await pause(6500);
  const added=(await ids()).filter(id=>!before.includes(id));
  const bodies=[];
  for(const n of added) bodies.push(await body(n));
  if(!bodies.some(b=>b==='still working\n')) throw Error('Slow file blocked another text open');
  if(!bodies.some(b=>b.includes('timed out'))) throw Error('Slow read did not show timeout: '+bodies.join('\n---\n'));
  const slow=await page.evaluate(()=>[window.slowReads,window.slowFlushes]);
  if(slow[0]!==1) throw Error('Stalled file was retried: '+slow);
  await page.evaluate(()=>window.releaseSlow());
  await pause(1000);
  id=await opened('/n/host/clip/small.txt');
  if(await body(id)!=='still working\n')throw Error('Late reply broke file opening');
  await close(id);
  await grab('plumber-timeout');
  console.log('Stalled 9P read times out; other opens remain responsive: PASS');
  if(errors.length) throw Error(errors.join('\n'));
} finally {
  console.log('Browser errors:',errors);
  console.log('Serial:',(await page.locator('#serial').textContent()).slice(-1600));
  await browser.close();
}
