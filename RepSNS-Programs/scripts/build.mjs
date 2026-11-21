import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises';

const read = (name) => readFile(new URL(`../${name}`, import.meta.url), 'utf8');
const json = async (name) => JSON.parse(await read(name));
const names = ['store', 'claude', 'index'];
const [home, coach, favicon, master, seated, sessions, prompts, ...modules] = await Promise.all([
  read('web/index.html'), read('web/coach.html'), read('web/favicon.svg'), json('data/master_plan.json'),
  json('data/wheelchair_upper_body.json'), json('data/sessions.json'), json('data/prompts.json'),
  ...names.map((name) => read(`worker/${name}.js`)),
]);
const assets = JSON.stringify({ home, coach, favicon, master, seated, sessions, prompts });
const source = modules.map((module) => module.replace(/^import .*;\r?\n/gm, '').replace(/^export (?!default)/gm, ''));
const server = new URL('../dist/server/', import.meta.url);
const metadata = new URL('../dist/.openai/', import.meta.url);
await Promise.all([mkdir(server, { recursive: true }), mkdir(metadata, { recursive: true })]);
await Promise.all([
  writeFile(new URL('index.js', server), `const assets = ${assets};\n\n${source.join('\n\n')}`),
  copyFile(new URL('../.openai/hosting.json', import.meta.url), new URL('hosting.json', metadata)),
]);
