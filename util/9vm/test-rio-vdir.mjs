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
try {
  await page.goto(url);
  await page.waitForFunction(() => document.querySelector('#serial').textContent.includes('halatha: starting Rio'), null, { timeout: 120000 });
  await pause(10000);
  await command('cat /dev/theme > /n/host/clip/seen');
  const palette = await hostText('clip/seen');
  if (!palette?.includes('dir 5555ff\n') || !palette.includes('exec 55ff55\n')) throw Error('Missing palette: '+palette);
  console.log('rio theme read: PASS', palette);
  await point(100, 100);
  await page.evaluate(() => window.emulator.bus.send('mouse-click', [false, false, true]));
  await pause(400);
  await grab('new-menu');
  await page.evaluate(() => window.emulator.bus.send('mouse-click', [false, false, false]));
  await pause(400);
  await point(40, 350);
  await page.evaluate(() => window.emulator.bus.send('mouse-click', [false, false, true]));
  await pause(200);
  for(let x=0; x<760; x+=20) { await move(20, 0); await pause(20); }
  for(let y=0; y<350; y+=10) { await move(0, 10); await pause(20); }
  await page.evaluate(() => window.emulator.bus.send('mouse-click', [false, false, false]));
  await pause(3000);
  await grab('vdir');
  console.log('New menu opened vdir');
  await point(690, 365);
  await page.evaluate(() => window.emulator.bus.send('mouse-click', [true, false, false]));
  await pause(100);
  await page.evaluate(() => window.emulator.bus.send('mouse-click', [false, false, false]));
  await pause(2000);
  await command('pwd > /n/host/clip/seen');
  console.log('Terminal cwd:', await hostText('clip/seen'));
  await grab('vdir-terminal');
  if (await hostText('clip/seen') !== '/\n') throw Error('Terminal here did not open at /');
  console.log('New menu and Terminal here: PASS');
  await page.evaluate(() => window.aspera.fs.mkdir("clip/dir with ' quote", true));
  await command("echo 'new -r 40 350 800 700 vdir /n/host/clip' > /dev/wctl");
  await pause(2000);
  await point(180, 391);
  await page.evaluate(() => window.emulator.bus.send('mouse-click', [false, false, true]));
  await pause(150);
  await page.evaluate(() => window.emulator.bus.send('mouse-click', [false, false, false]));
  await pause(1000);
  await grab('quoted-path');
  await point(690, 365);
  await buttons(true); await pause(150); await buttons(false);
  await pause(2000);
  await command('pwd > /n/host/clip/seen');
  if (await hostText('clip/seen') !== "/n/host/clip/dir with ' quote\n") throw Error('Quoted cwd failed: '+await hostText('clip/seen'));
  console.log('Terminal here with spaces/apostrophe: PASS');
} finally {
  console.log('Browser errors:', errors);
  console.log('Serial tail:', (await page.locator('#serial').textContent()).slice(-1000));
  await browser.close();
}
