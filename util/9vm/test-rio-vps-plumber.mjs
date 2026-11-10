import { chromium } from '../../../halatha/node_modules/playwright/index.mjs';
const url=process.argv[2] || 'http://127.0.0.1:7091/examples/basic-vm-harvey-vps.html';
// Optional --baseline=/path/to/old-kernel reproduces the missing service.
const baseline=process.argv.find(a=>a.startsWith('--baseline='))?.slice(11);
const browser=await chromium.launch({args:['--no-sandbox']});
const page=await browser.newPage({viewport:{width:1200,height:950}});
const errors=[], logs=[];
page.on('pageerror',e=>errors.push(e.message));
page.on('console',msg=>{const t=msg.text();logs.push(t);if(/vps:.*(plumb|Rio|root mounted)/.test(t))console.log(t);});
try {
 if(baseline) await page.route('**/local/aspera/harvey-vps.32bit?*',r=>r.fulfill({path:baseline}));
 await page.goto(url,{waitUntil:'domcontentloaded',timeout:60000});
 await page.waitForFunction(()=>document.querySelector('#harvey')?.task?.taskRoot && (!document.querySelector('#status') || /Guest ready/.test(document.querySelector('#status').textContent)),null,{timeout:240000});
 await page.waitForTimeout(18000);
  const pause = ms => page.waitForTimeout(ms);
  // The startup script leaves the right terminal current.
  console.log('Using the initial right terminal...');
  const canvas = await page.waitForSelector('canvas, wanix-vm canvas');
  await canvas.focus();

  const typeCmd = async (str) => {
    for (const ch of str) {
      if (ch === ' ') await page.keyboard.press('Space');
      else if (ch === '/') await page.keyboard.press('Slash');
      else if (ch === ';') await page.keyboard.press('Semicolon');
      else if (ch === '>') await page.keyboard.press('Shift+Period');
      else if (ch === '[') await page.keyboard.press('BracketLeft');
      else if (ch === ']') await page.keyboard.press('BracketRight');
      else if (ch === '&') await page.keyboard.press('Shift+Digit7');
      else if (ch === '.') await page.keyboard.press('Period');
      else if (ch >= 'A' && ch <= 'Z') await page.keyboard.press(`Shift+Key${ch}`);
      else if (ch >= 'a' && ch <= 'z') await page.keyboard.press(`Key${ch.toUpperCase()}`);
      else if (ch >= '0' && ch <= '9') await page.keyboard.press(`Digit${ch}`);
      else await page.keyboard.press(ch);
      await pause(60);
    }
    await page.keyboard.press('Enter');
    await pause(2000);
  };


 const rootWrite=(name,data)=>page.evaluate(async ([name,data])=>{
  const root=document.querySelector('#harvey').task.taskRoot;
  await root.writeFile('ram/tmp/'+name,new TextEncoder().encode(data));
 },[name,data]);
 const rootRead=name=>page.evaluate(async name=>new TextDecoder().decode(await document.querySelector('#harvey').task.taskRoot.readFile('ram/tmp/'+name)),name);
 await rootWrite('plumber-diag','');
 await typeCmd('ls /mnt/plumb > /tmp/plumber-diag >[2=1]');
 const ports=await rootRead('plumber-diag');console.log('Plumbing ports:',ports);
 if(baseline){
  if(ports.includes('/mnt/plumb/send'))throw Error('Baseline unexpectedly has plumber');
  console.log('REPRODUCED: VPS boot has no plumbing send endpoint');
 }else{
  if(!ports.includes('/mnt/plumb/send'))throw Error('Plumber not mounted');
  await page.evaluate(async ()=>{
   const root=document.querySelector('#harvey').task.taskRoot;
   await root.makeDirAll('ram/tmp/vdir-open-test');
   await root.writeFile("ram/tmp/vdir-open-test/a file's name.txt",new TextEncoder().encode('original\n'));
  });
  await typeCmd("echo 'new -r 40 440 800 740 vdir /tmp/vdir-open-test' > /dev/wctl");
  await pause(1500);
  const rect=await canvas.boundingBox();
  const size=await canvas.evaluate(c=>[c.width,c.height]);
  await page.mouse.click(rect.x+180*rect.width/size[0],rect.y+482*rect.height/size[1],{button:'right'});
  await pause(3500);
  await canvas.focus();
  await page.keyboard.press('Control+End');
  for(const ch of 'edited')await page.keyboard.press('Key'+ch.toUpperCase());
  await page.keyboard.press('Enter');
  await page.keyboard.press('Control+s');
  await page.waitForFunction(async ()=>{
   const root=document.querySelector('#harvey').task.taskRoot;
   return new TextDecoder().decode(await root.readFile("ram/tmp/vdir-open-test/a file's name.txt"))==='original\nedited\n';
  },null,{timeout:30000});
  console.log('PASS: VPS boot -> inherited plumber -> vdir right-click -> rio-open -> editor -> exact save');
 }
 await page.screenshot({path:'/tmp/rio-vps-plumber-'+(baseline?'before':'after')+'.png'});
 if(errors.length)throw Error(errors.join('\n'));
} catch(e){
 await page.screenshot({path:'/tmp/rio-vps-plumber-failure.png'}).catch(()=>{});
 console.log('Recent console:',logs.slice(-15).join('\n'));
 throw e;
} finally {await browser.close();}
