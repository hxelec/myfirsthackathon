import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { Script } from 'node:vm';
import { localDatabase } from './local-database.mjs';
import './build.mjs';

const { default: worker } = await import('../dist/server/index.js');
const origin = 'https://repsns.example';
const env = { DB: localDatabase() };
function request(path, { cookie, data, from = origin, runtime = env, hasSignal = true } = {}) {
  const incoming = new Request(`${origin}${path}`, {
    method: data === undefined ? 'GET' : 'POST',
    headers: { ...(cookie ? { Cookie: cookie } : {}), ...(data === undefined ? {} : { Origin: from, 'Content-Type': 'application/json' }) },
    body: data === undefined ? undefined : JSON.stringify(data),
  });
  if (!hasSignal) Object.defineProperty(incoming, 'signal', { value: undefined });
  return worker.fetch(incoming, runtime);
}

const home = await request('/');
assert.equal(home.status, 200);
assert.match(await home.text(), /My workout plans/);
assert.match(home.headers.get('set-cookie'), /HttpOnly; SameSite=Lax; Secure/);
const cookie = home.headers.get('set-cookie').split(';')[0];
assert.equal((await request('/coach', { cookie })).status, 200);
assert.equal((await request('/favicon.svg')).status, 200);
for (const page of ['index', 'coach']) {
  const html = await readFile(new URL(`../web/${page}.html`, import.meta.url), 'utf8');
  for (const [, script] of html.matchAll(/<script>([\s\S]*?)<\/script>/g)) new Script(script, { filename: `${page}.html` });
}

const library = async (device = cookie) => (await (await request('/api/programs', { cookie: device })).json()).programs;
const change = (data) => request('/api/programs', { cookie, data });
const original = await library();
assert.equal(original.length, 2);
const [master, seated] = original;
assert.equal(master.is_master, true);
assert.equal((await (await request('/api/templates', { cookie })).json()).templates.length, 2);
assert.equal((await change({ action: 'save', program: master.program, name: 'Copy' })).status, 200);
assert.equal((await change({ action: 'save', program: seated.program, name: 'Seated copy' })).status, 200);
const updates = await Promise.all([
  change({ action: 'rename', id: master.id, name: 'Updated plan' }),
  change({ action: 'master', id: seated.id }),
]);
assert(updates.every((response) => response.ok));
const updated = await library();
assert.equal(updated.find((plan) => plan.id === master.id).name, 'Updated plan');
assert.deepEqual(updated.filter((plan) => plan.is_master).map((plan) => plan.id), [seated.id]);
const freshCookie = (await request('/')).headers.get('set-cookie').split(';')[0];
const freshPlans = await library(freshCookie);
assert.equal(freshPlans.length, 2);
assert(freshPlans.every((plan) => !updated.some(({ id }) => id === plan.id)));
assert.equal((await request('/api/programs', { cookie, from: 'https://another.example', data: { action: 'delete', id: master.id } })).status, 403);
for (const plan of updated.slice(1)) assert.equal((await change({ action: 'delete', id: plan.id })).status, 200);
assert.equal((await change({ action: 'delete', id: master.id })).status, 400);
assert.equal((await change({ action: 'load_template', template_id: 'wheelchair-upper-body' })).status, 200);
assert.deepEqual(await (await request('/api/status')).json(), { live: false, model: 'claude-sonnet-5-5' });
const chat = { messages: [{ role: 'user', content: 'What is my master plan?' }] };
assert.equal((await request('/api/chat', { cookie, data: chat })).status, 503);

const fakeRuntime = { ...env, ANTHROPIC_API_KEY: 'test-key' };
const realFetch = globalThis.fetch;
let sawToolResult = false;
let sawAttachment = false;
globalThis.fetch = async (url, options) => {
  assert.equal(url, 'https://api.anthropic.com/v1/messages');
  assert.equal(options.redirect, 'manual');
  assert(options.signal instanceof AbortSignal);
  const body = JSON.parse(options.body);
  assert.equal(body.model, 'claude-sonnet-5-5');
  assert.equal(body.output_config.format.type, 'json_schema');
  if (!body.tools) return Response.json({ stop_reason: 'end_turn', content: [{ type: 'text', text: JSON.stringify(master.program) }] });
  assert.match(body.messages[0].content, /Updated plan/);
  const last = body.messages.at(-1).content;
  sawAttachment ||= Array.isArray(last) && last.some((block) => block.type === 'document');
  if (Array.isArray(last) && last[0].type === 'tool_result') {
    sawToolResult = true;
    assert.equal(JSON.parse(last[0].content).saved.name, 'Renamed by coach');
    return Response.json({ stop_reason: 'end_turn', content: [{ type: 'text', text: JSON.stringify({ kind: 'program', program_id: master.id, text: 'Plan renamed.' }) }] });
  }
  return Response.json({ stop_reason: 'tool_use', content: [{ type: 'tool_use', id: 'toolu_test', name: 'rename_program', input: { program_id: master.id, new_name: 'Renamed by coach' } }] });
};
try {
  const generated = await request('/api/program', { cookie, runtime: fakeRuntime, hasSignal: false, data: { answers: { goal: 'Strength', days_per_week: 3 } } });
  assert.equal(generated.status, 200);
  assert.equal((await generated.json()).programs.length, 3);
  const attached = { messages: [{ role: 'user', content: [{ type: 'text', text: 'Rename my plan.' }, { type: 'document', title: 'plan.txt', source: { type: 'text', media_type: 'text/plain', data: 'My strength plan' } }] }] };
  const coached = await request('/api/chat', { cookie, runtime: fakeRuntime, data: attached });
  assert.equal(coached.status, 200);
  const result = await coached.json();
  assert.equal(result.kind, `program:${master.id}`);
  assert.deepEqual(result.changed, [master.id]);
  assert(sawAttachment && sawToolResult);
  assert.equal((await library()).find((plan) => plan.id === master.id).name, 'Renamed by coach');
} finally {
  globalThis.fetch = realFetch;
}
console.log('Passed: pages, templates, plan changes, concurrent updates, browser isolation, and simulated Claude generation and coach tool use.');
