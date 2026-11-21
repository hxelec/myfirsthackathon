import { createServer } from 'node:http';
import { mkdir } from 'node:fs/promises';
import { Readable } from 'node:stream';
import { localDatabase } from './local-database.mjs';
import './build.mjs';

const { default: worker } = await import('../dist/server/index.js');
const port = Number(process.env.REPSNS_PREVIEW_PORT || 8770);
const origin = `http://127.0.0.1:${port}`;
const dataDirectory = new URL('../.local/', import.meta.url);
await mkdir(dataDirectory, { recursive: true });
const env = { ...process.env, DB: localDatabase(new URL('preview.sqlite', dataDirectory)) };
createServer(async (incoming, outgoing) => {
  const abort = new AbortController();
  outgoing.on('close', () => abort.abort());
  const options = { method: incoming.method, headers: incoming.headers, signal: abort.signal };
  if (!['GET', 'HEAD'].includes(options.method)) Object.assign(options, { body: Readable.toWeb(incoming), duplex: 'half' });
  try {
    const response = await worker.fetch(new Request(`${origin}${incoming.url}`, options), env);
    outgoing.writeHead(response.status, Object.fromEntries(response.headers));
    if (incoming.method === 'HEAD' || !response.body) outgoing.end();
    else Readable.fromWeb(response.body).pipe(outgoing);
  } catch {
    outgoing.writeHead(500).end('Preview request could not be completed.');
  }
}).listen(port, '127.0.0.1', () => console.log(`RepSNS preview: ${origin}`));
