#!/usr/bin/env node
// Render a prototype HTML to a 1440x900 PNG (matches 01-library-browse.png size).
// Usage: node scripts/prototypes/render-prototype.mjs <html> <out.png>
// Renders at 2x for crispness, downscales to 1440x900 (the confirmed PNG spec).
import { chromium } from '/home/cui/.cache/persona-ui-shots/node_modules/playwright/index.mjs';
import { execFile } from 'node:child_process';
import { resolve } from 'node:path';

const EXE = '/home/cui/.cache/ms-playwright/chromium_headless_shell-1234/chrome-headless-shell-linux64/chrome-headless-shell';
const [html, out] = process.argv.slice(2).map(p => resolve(p));
if (!html || !out) { console.error('usage: render-prototype.mjs <html> <out.png>'); process.exit(2); }

const browser = await chromium.launch({ executablePath: EXE, args: ['--no-sandbox'] });
const page = await browser.newPage({ viewport: { width: 1440, height: 900 }, deviceScaleFactor: 2 });
await page.goto('file://' + html, { waitUntil: 'load' });
await page.waitForTimeout(150);
const tmp = out + '.@2x.png';
await page.screenshot({ path: tmp, clip: { x: 0, y: 0, width: 1440, height: 900 } });
await browser.close();
// Downscale 2880x1800 -> 1440x900
execFile('python3', ['-c', `
from PIL import Image
im = Image.open(${JSON.stringify(tmp)})
im.resize((1440, 900), Image.LANCZOS).save(${JSON.stringify(out)})
import os; os.remove(${JSON.stringify(tmp)})
`, ], (err) => {
  if (err) { console.error('downscale failed', err); process.exit(1); }
  console.log('wrote', out);
});
