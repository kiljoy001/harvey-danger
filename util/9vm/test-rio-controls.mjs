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
const click = async (x,y) => { await point(x,y); await buttons(true); await pause(100); await buttons(false); await pause(1200); };
const footer = async (x,y) => page.locator('#vga canvas').evaluate((c,[x,y]) => Array.from(c.getContext('2d').getImageData(x-268,y-29,264,25).data),[x,y]);
const visible = data => {
  let black=0, white=0;
  for(let i=0;i<data.length;i+=4){
    if(data[i]<40 && data[i+1]<40 && data[i+2]<40) black++;
    if(data[i]>245 && data[i+1]>245 && data[i+2]>245) white++;
  }
  if(black<250 || white<1800) throw Error(`Controls lost contrast: ${black} black, ${white} white pixels`);
};
const same = (a,b) => {
  if(a.length!==b.length || a.some((v,i)=>v!==b[i])) throw Error('Controls changed after redraw/restore');
};
try {
  await page.goto(url);
  await page.waitForFunction(() => document.querySelector('#serial').textContent.includes('halatha: starting Rio'), null, { timeout: 120000 });
  await pause(10000);
  await command("echo 'new -r 40 350 800 700 vdir /n/host' > /dev/wctl");
  await pause(2000);
  await point(100,200);
  const original=await footer(800,700); visible(original);
  await grab('controls-initial');
  // Navigation redraws the entire graphical client.
  await point(120,391);
  await page.evaluate(() => window.emulator.bus.send('mouse-click',[false,false,true]));
  await pause(100);
  await page.evaluate(() => window.emulator.bus.send('mouse-click',[false,false,false]));
  await pause(1000);
  await click(103,365);
  await point(100,200);
  same(original,await footer(800,700));
  // Inactive buttons retain contrast and activate with a single click.
  await click(600,110); await point(100,200);
  same(original,await footer(800,700));
  await click(665,684); await point(100,200);
  visible(await footer(1024,768));
  await grab('controls-max');
  await click(888,752); await point(100,200);
  same(original,await footer(800,700));
  console.log('vdir redraw, inactive contrast, maximize/restore: PASS');
  // A canceled close must leave the window intact.
  await point(750,684); await buttons(true); await pause(100);
  await point(100,200); await buttons(false); await pause(1000);
  same(original,await footer(800,700));
  // Resize using Rio's actual border gesture.
  await point(798,698); await buttons(true); await pause(100);
  await move(100,20); await pause(150); await buttons(false); await pause(2000);
  await point(100,200);
  visible(await footer(900,720));
  await grab('controls-resize');
  console.log('Border resize and canceled close: PASS');
  await click(850,704); await point(100,200);
  const closed=await footer(900,720);
  if(closed.filter((v,i)=>i%4===0 && v<40).length>100) throw Error('Close did not remove vdir');
  // Terminal output/refresh must leave its controls intact too.
  await click(600,110);
  await command('seq 1 30'); await point(100,200);
  visible(await footer(980,320));
  await command("echo 'resize -r 460 100 960 420' > /dev/wctl");
  await point(100,200); visible(await footer(960,420));
  await grab('controls-terminal');
  await command("echo 'new -r 40 350 800 700 vdir /n/host' > /dev/wctl");
  await pause(2000);
  await click(575,684); await point(100,200);
  const hidden=await footer(800,700);
  if(hidden.filter((v,i)=>i%4===0 && v<40).length>100) throw Error('Hide did not remove vdir');
  await click(600,150);
  await command('ls /dev/wsys > /n/host/clip/seen');
  const ids=(await hostText('clip/seen')).trim().split(/\s+/);
  if(ids.length!==2) throw Error('Hide deleted the window, or Close kept it: '+ids);
  console.log('Close, hide, terminal output and terminal resize: PASS');
  if(errors.length) throw Error(errors.join('\n'));
} finally {
  console.log('Browser errors:', errors);
  console.log('Serial tail:', (await page.locator('#serial').textContent()).slice(-1000));
  await browser.close();
}
