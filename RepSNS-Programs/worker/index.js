import { AppError, PlanStore, templatesFor } from './store.js';
import { chatWithCoach, claudeModel, cleanAnswers, cleanHistory, generatePlan, isObject, programPrompt, validPlan } from './claude.js';

const templates = templatesFor(assets);
const maxBody = 24 * 1024 * 1024;

function identify(request, url) {
  const cookie = request.headers.get('Cookie')?.split(';').map((part) => part.trim())
    .find((part) => /^repsns_uid=[a-f0-9]{32}$/.test(part));
  const id = cookie?.slice('repsns_uid='.length) || crypto.randomUUID().replaceAll('-', '');
  const secure = url.protocol === 'https:' ? '; Secure' : '';
  const user = request.headers.get('oai-authenticated-user-id') || '';
  return {
    key: `${user}:${id}`,
    cookie: cookie ? null : `repsns_uid=${id}; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax${secure}`,
  };
}

function makeResponse(body, status, type, cookie) {
  const headers = new Headers({
    'Content-Type': type, 'Cache-Control': 'private, no-store',
    'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'same-origin',
  });
  if (cookie) headers.set('Set-Cookie', cookie);
  return new Response(body, { status, headers });
}

async function readBody(request) {
  const invalid = () => new AppError(400, 'Could not read that request.');
  const tooLarge = () => new AppError(413, 'Those files are too large to send together. Try fewer or smaller files.');
  if (request.headers.get('Content-Type')?.split(';')[0].trim() !== 'application/json' || !request.body) throw invalid();
  if (Number(request.headers.get('Content-Length')) > maxBody) throw tooLarge();
  const reader = request.body.getReader();
  const decoder = new TextDecoder();
  let bytes = 0;
  let body = '';
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      bytes += value.byteLength;
      if (bytes > maxBody) {
        await reader.cancel();
        throw tooLarge();
      }
      body += decoder.decode(value, { stream: true });
    }
    const data = JSON.parse(body + decoder.decode());
    if (!isObject(data)) throw invalid();
    return data;
  } catch (error) {
    if (error instanceof AppError) throw error;
    throw invalid();
  } finally {
    reader.releaseLock();
  }
}

async function saveGenerated(store, env, signal, data) {
  const answers = cleanAnswers(data.answers);
  const prompt = programPrompt(answers, data.previous_program, data.refinement);
  const saved = (await store.list()).find(({ id }) => id === data.id);
  const program = await generatePlan(env, signal, assets.prompts.program, prompt);
  if (signal?.aborted) throw new AppError(409, 'The request was stopped.');
  const { item, programs } = await store.change({ action: 'save', program, answers, id: saved?.id, name: saved?.name, master: data.master === true }, { reuseMissingId: true });
  return { program, item, programs };
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;
    const device = identify(request, url);
    const json = (body, status = 200) => makeResponse(JSON.stringify(body), status, 'application/json; charset=utf-8', device.cookie);
    const html = (body) => makeResponse(request.method === 'HEAD' ? null : body, 200, 'text/html; charset=utf-8', device.cookie);
    const store = new PlanStore(env.DB, device.key, templates);
    try {
      if (request.method === 'GET' || request.method === 'HEAD') {
        if (path === '/' || path === '/index.html') return html(assets.home);
        if (path === '/coach' || path === '/coach/' || path === '/coach.html') return html(assets.coach);
        if (path === '/favicon.svg') return makeResponse(assets.favicon, 200, 'image/svg+xml; charset=utf-8');
        if (path === '/api/status') return json({ live: Boolean(env.ANTHROPIC_API_KEY?.trim()), model: claudeModel(env) });
        if (path === '/api/programs') return json({ programs: await store.list() });
        if (path === '/api/templates') return json({ templates: templates.map(({ id, name, program }) => ({ id, name, title: program.title, summary: program.summary, days: program.days.length })) });
        return json({ error: 'Page not found.' }, 404);
      }

      if (request.method !== 'POST' || !['/api/programs', '/api/program', '/api/chat'].includes(path)) {
        return json({ error: 'Page not found.' }, 404);
      }
      if (request.headers.get('Origin') !== url.origin || request.headers.get('Sec-Fetch-Site') === 'cross-site') {
        return json({ error: 'Open the RepSNS site before making this request.' }, 403);
      }
      const data = await readBody(request);
      if (path === '/api/programs') {
        if (data.action === 'save' && !validPlan(data.program)) throw new AppError(400, 'This plan is incomplete.');
        return json(await store.change({ ...data, answers: cleanAnswers(data.answers), master: data.master === true }));
      }
      if (path === '/api/program') return json(await saveGenerated(store, env, request.signal, data));
      return json(await chatWithCoach(store, env, request.signal, assets, cleanHistory(data.messages)));
    } catch (error) {
      if (error instanceof AppError) return json({ error: error.message }, error.status);
      console.error('RepSNS request failed', request.method, path, error.name, error.message);
      return json({ error: 'RepSNS could not complete that request. Please try again.' }, 500);
    }
  },
};
