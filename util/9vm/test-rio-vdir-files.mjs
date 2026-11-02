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
  await page.waitForFunction(() => document.querySelector('#serial').textContent.includes('halatha: starting Rio'), null, {timeout:120000});
  await pause(10000);
  await command('ls /n/host > /n/host/clip/seen');
  const host = await hostText('clip/seen'); console.log('Host entries:',host);
  if(!host.includes('/n/host/clip') || host.includes('/n/host/bin') || host.includes('/n/host/n\n')) throw Error('Host root not bounded');
  await command('ls /n/host/n/host > /n/host/clip/seen >[2=1]');
  console.log('Cycle probe:', await hostText('clip/seen'));
  if(!(await hostText('clip/seen'))?.includes('does not exist') && !(await hostText('clip/seen'))?.includes('not found') && !(await hostText('clip/seen'))?.includes('no such')) throw Error('Nested host still resolves');
  await command('ls /mnt/plumb > /n/host/clip/seen');
  console.log('Plumber:',await hostText('clip/seen'));
  if(!(await hostText('clip/seen'))?.includes('send')) throw Error('Plumber not mounted');
  await page.evaluate(() => {
    window.aspera.fs.mkdir('clip/open-tests', true);
    window.aspera.fs.addFile("clip/open-tests/a file's name.txt", 'original\n', {writable:true,mode:0o600});
  });
  await command("echo 'new -r 40 350 800 700 vdir /n/host/clip/open-tests' > /dev/wctl");
  await pause(2000);
  await point(180,391);
  await page.evaluate(() => window.emulator.bus.send('mouse-click',[false,false,true]));
  await pause(150);
  await page.evaluate(() => window.emulator.bus.send('mouse-click',[false,false,false]));
  await pause(3000);
  await grab('file-open');
  await page.keyboard.press('Control+End');
  await text('edited'); await page.keyboard.press('Enter');
  await page.keyboard.press('Control+s');
  await waitFile("clip/open-tests/a file's name.txt", 'original\nedited\n');
  console.log('vdir -> plumber -> rio editor -> save, quoted filename: PASS');
  await grab('file-saved');
} finally {
  console.log('Browser errors:',errors);
  console.log('Serial:',(await page.locator('#serial').textContent()).slice(-1600));
  await browser.close();
}
