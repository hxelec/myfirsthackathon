import { AppError, getPlan, planSummary } from './store.js';

const string = { type: 'string' };
const integer = { type: 'integer' };
const enumOf = (...values) => ({ type: typeof values[0] === 'number' ? 'integer' : 'string', enum: values });
const arrayOf = (items) => ({ type: 'array', items });
const objectOf = (properties) => ({ type: 'object', properties, required: Object.keys(properties), additionalProperties: false });
const strings = arrayOf(string);
const velocityLoss = enumOf(0, 20, 40, 60);

const programSchema = objectOf({
  title: string, summary: string, assumptions: strings, velocity_loss_explainer: string,
  days: arrayOf(objectOf({
    name: string, focus: string, estimated_minutes: integer,
    warmup: arrayOf(objectOf({ activity: string, prescription: string })),
    exercises: arrayOf(objectOf({
      name: string,
      muscle_group: enumOf('Quads', 'Hamstrings & glutes', 'Back', 'Chest', 'Shoulders', 'Arms', 'Core', 'Calves', 'Full body', 'Other'),
      body_region: enumOf('Upper', 'Lower', 'Full', 'Core'), warmup_sets: string, working_sets: integer,
      rep_range: string, target_vl_percent: velocityLoss, rest_seconds: integer, notes: string,
    })),
    cooldown: string,
  })),
  progression_rules: strings,
  block_weeks: arrayOf(objectOf({ week: integer, working_sets_per_week: integer, target_vl_percent: velocityLoss, focus: string })),
  adaptations: strings,
  nutrition: objectOf({ pre_workout: strings, post_workout: strings, hydration: string }),
  clarifying_questions: strings,
});

const fields = {
  days_per_week: 'Training days per week', training_days: 'Preferred days', session_minutes: 'Time available per gym session (minutes)',
  goal: 'Main goal', experience: 'Training experience', rep_range: 'Preferred rep range', equipment: 'Equipment available',
  include: 'Exercises they want included', avoid: 'Exercises they want to avoid', limitations: 'Disabilities, injuries or mobility considerations',
  volume_capacity: 'Volume / recovery capacity', vl_preference: 'Velocity loss preference', age_range: 'Age range', notes: 'Anything else from the user',
};

export const isObject = (value) => value !== null && typeof value === 'object' && !Array.isArray(value);
export const claudeModel = (env) => env.ANTHROPIC_MODEL?.trim() || 'claude-sonnet-5-5';

export function cleanAnswers(raw) {
  if (!isObject(raw)) return {};
  const answers = {};
  for (const key of Object.keys(fields)) {
    const value = raw[key];
    if (typeof value === 'string' || Number.isFinite(value)) answers[key] = String(value).trim().slice(0, 2000);
    if (Array.isArray(value)) {
      answers[key] = value.filter((item) => typeof item === 'string').slice(0, 7).map((item) => item.slice(0, 200));
    }
  }
  return answers;
}

function matchesSchema(value, schema) {
  if (schema.enum) return schema.enum.includes(value);
  if (schema.type === 'string') return typeof value === 'string';
  if (schema.type === 'integer') return Number.isSafeInteger(value);
  if (schema.type === 'array') return Array.isArray(value) && value.every((item) => matchesSchema(item, schema.items));
  return isObject(value) && schema.required.every((key) => matchesSchema(value[key], schema.properties[key]));
}

export function validPlan(value) {
  return matchesSchema(value, programSchema) && value.days.length > 0;
}

export function programPrompt(answers, previous, change) {
  const lines = Object.entries(answers).filter(([, value]) => String(value).trim())
    .map(([key, value]) => `- ${fields[key]}: ${Array.isArray(value) ? value.join(', ') : value}`);
  if (!lines.length) throw new AppError(400, 'Could not read your answers. Please check the form.');
  let prompt = `Write a training program for this user.\n<user_answers>\n${lines.join('\n')}\n</user_answers>`;
  if (previous != null) {
    if (!validPlan(previous) || typeof change !== 'string' || !change.trim()) {
      throw new AppError(400, 'Could not read that change. Enter a refinement and try again.');
    }
    prompt += `\n\nRevise this program and re-check the session time. Keep the parts that still fit.\n<current_program>\n${JSON.stringify(previous).slice(0, 60_000)}\n</current_program>\n<requested_change>\n${change.trim().slice(0, 2000)}\n</requested_change>`;
  }
  return prompt;
}

const apiErrors = {
  401: 'Claude rejected the hosted API key. The site owner needs to update it.',
  403: 'The Claude account cannot access this model. The site owner can check model access.',
  404: 'The configured Claude model is unavailable. The site owner can update the model.',
  429: 'Claude is rate limited. Wait a moment and try again.',
};

async function anthropicMessage(env, signal, { effort, ...payload }) {
  const key = env.ANTHROPIC_API_KEY?.trim();
  if (!key) throw new AppError(503, 'The AI coach and builder need an Anthropic API key in the Site settings. You can still use the pre-made plans.');
  const { output_config, ...body } = payload;
  const timeout = AbortSignal.timeout(110_000);
  let response;
  try {
    response = await fetch('https://api.anthropic.com/v1/messages', {
      method: 'POST', redirect: 'manual', signal: signal ? AbortSignal.any([signal, timeout]) : timeout,
      headers: { 'Content-Type': 'application/json', 'x-api-key': key, 'anthropic-version': '2023-06-01' },
      body: JSON.stringify({ model: claudeModel(env), ...body, output_config: { effort, ...output_config } }),
    });
  } catch (error) {
    if (signal?.aborted) throw new AppError(409, 'The request was stopped.');
    console.warn('Anthropic connection failed', error.name, { timedOut: timeout.aborted });
    throw new AppError(502, timeout.aborted ? 'Claude did not reply in time. Please try again.' : 'Could not reach Claude. Please try again.');
  }
  if (!response.ok) {
    console.warn('Anthropic response', response.status, response.headers.get('request-id'));
    await response.body?.cancel();
    throw new AppError(response.status === 429 ? 429 : 502, apiErrors[response.status] || 'Claude could not reply. Please try again.');
  }
  const message = await response.json();
  if (['max_tokens', 'model_context_window_exceeded'].includes(message.stop_reason)) {
    throw new AppError(502, 'Claude ran out of room for this reply. Try fewer details or a shorter request.');
  }
  return message;
}

function parseReply(message) {
  try {
    return JSON.parse(message.content.filter((block) => block.type === 'text').map((block) => block.text).join(''));
  } catch {
    throw new AppError(502, 'Claude did not return a usable reply. Please try again.');
  }
}

export async function generatePlan(env, signal, system, prompt) {
  const message = await anthropicMessage(env, signal, {
    max_tokens: 16_000, system, effort: env.ANTHROPIC_EFFORT || 'medium',
    messages: [{ role: 'user', content: prompt }],
    output_config: { format: { type: 'json_schema', schema: programSchema } },
  });
  if (message.stop_reason === 'refusal') throw new AppError(422, 'Claude declined to write this plan. Try rephrasing your answers.');
  const program = parseReply(message);
  if (!validPlan(program)) throw new AppError(502, 'Claude returned an incomplete plan. Please try again.');
  return program;
}

const imageTypes = new Set(['image/png', 'image/jpeg', 'image/webp', 'image/gif']);
const goodText = (value, max) => typeof value === 'string' && value.trim() && value.length <= max;

function validBase64(data, bytes) {
  if (typeof data !== 'string' || !data.length || data.length % 4 || data.length > Math.ceil(bytes / 3) * 4) return false;
  const padding = data.endsWith('==') ? 2 : Number(data.endsWith('='));
  return (data.length / 4 * 3 - padding) <= bytes && !/[^A-Za-z0-9+/]/.test(data.slice(0, data.length - padding));
}

function cleanAttachment(block) {
  const source = block.source;
  if (!isObject(source)) return null;
  const { type, media_type, data } = source;
  if (block.type === 'image' && type === 'base64' && imageTypes.has(media_type) && validBase64(data, 5 * 1024 * 1024)) {
    return { type: 'image', source: { type, media_type, data } };
  }
  if (block.type !== 'document') return null;
  const isPDF = type === 'base64' && media_type === 'application/pdf' && validBase64(data, 10 * 1024 * 1024) && atob(data.slice(0, 8)).startsWith('%PDF');
  const isText = type === 'text' && media_type === 'text/plain' && goodText(data, 150_000);
  if (!isPDF && !isText) return null;
  const title = typeof block.title === 'string' ? block.title.trim().slice(0, 200) : '';
  return { type: 'document', title: title || 'Attached file', source: { type, media_type, data } };
}

export function cleanHistory(raw) {
  const invalid = () => { throw new AppError(400, 'Could not read this chat message or one of its files.'); };
  if (!Array.isArray(raw) || !raw.length || raw.length > 16) invalid();
  let attachments = 0;
  const history = raw.map((message) => {
    if (!isObject(message) || !['user', 'assistant'].includes(message.role)) invalid();
    const { role, content } = message;
    if (goodText(content, 20_000)) return { role, content };
    if (role !== 'user' || !Array.isArray(content) || !content.length || content.length > 12) invalid();
    const blocks = content.map((block) => {
      if (!isObject(block)) invalid();
      if (block.type === 'text' && goodText(block.text, 20_000)) return { type: 'text', text: block.text };
      const attachment = cleanAttachment(block);
      if (!attachment || ++attachments > 8) invalid();
      return attachment;
    });
    if (!blocks.some((block) => block.type === 'text')) invalid();
    return { role, content: blocks };
  });
  if (history[0].role === 'assistant') history.shift();
  if (history.at(-1)?.role !== 'user') invalid();
  return history;
}

const idField = { type: 'string', description: 'Saved program ID from list_programs.' };
const boolean = { type: 'boolean' };
const tool = (name, description, fields) => ({ name, description, input_schema: objectOf(fields), strict: true });
const coachTools = [
  tool('list_programs', 'List saved programs with IDs, master status and day summaries.', {}),
  tool('get_program', 'Read a saved program, including days, exercises, warm-up, velocity loss, rest and nutrition.', { program_id: idField }),
  tool('revise_program', 'Revise a saved program in place, or create a copy when requested. Use an empty new_name to keep the current name on a copy.', {
    program_id: idField, change_request: string, save_as_new: boolean, new_name: string,
  }),
  tool('create_program', 'Create and save a program from the known preferences and requirements.', {
    name: string, requirements: string, make_master: boolean,
  }),
  tool('rename_program', 'Rename a saved program.', { program_id: idField, new_name: string }),
  tool('set_master_program', 'Make a saved program the master program.', { program_id: idField }),
  tool('delete_program', 'Delete a saved program after the user has confirmed by name in the chat.', { program_id: idField }),
];

async function runCoachTool(store, env, signal, prompts, { name, input }, changed) {
  const plans = await store.list();
  if (name === 'list_programs') return plans.map(planSummary);
  if (name === 'get_program') return getPlan(plans, input.program_id);

  let change;
  if (name === 'create_program') {
    const answers = cleanAnswers({ notes: input.requirements });
    const master = plans.find((plan) => plan.is_master);
    const reference = master ? `\n\nUse the following plan for style and context where compatible with the new requirements.\n<master_plan>\n${JSON.stringify(master.program).slice(0, 60_000)}\n</master_plan>` : '';
    const program = await generatePlan(env, signal, prompts.program, programPrompt(answers) + reference);
    change = { action: 'save', name: input.name, answers, master: input.make_master, program };
  } else if (name === 'revise_program') {
    const item = getPlan(plans, input.program_id);
    const answers = Object.entries(item.answers).some(([, value]) => String(value).trim()) ? item.answers : { notes: 'See the current program.' };
    const program = await generatePlan(env, signal, prompts.program, programPrompt(answers, item.program, input.change_request));
    change = input.save_as_new
      ? { action: 'save', name: input.new_name || `${item.name} (copy)`, answers, program }
      : { action: 'save', id: item.id, name: item.name, program };
  } else {
    const actions = { rename_program: 'rename', set_master_program: 'master', delete_program: 'delete' };
    change = { action: actions[name], id: input.program_id, name: input.new_name };
  }

  if (signal?.aborted) throw new AppError(409, 'The request was stopped.');
  const { item } = await store.change(change);
  if (change.action === 'delete') return { deleted: item.name };
  changed.add(item.id);
  return { saved: planSummary(item), note: 'The change is saved. Reply with kind "program" and this program ID.' };
}

const coachSchema = objectOf({ text: string, kind: enumOf('program', 'chart', 'chart-all', 'table', 'none'), program_id: string });

export async function chatWithCoach(store, env, signal, { prompts, sessions }, history) {
  const plans = await store.list();
  const master = plans.find((plan) => plan.is_master);
  const context = JSON.stringify({ sample_sessions: sessions, saved_programs: plans.map(planSummary), current_master: master });
  const messages = [
    { role: 'user', content: `<app_context>\n${context}\n</app_context>` },
    { role: 'assistant', content: 'I have the current master plan and saved programs. I will use the current master plan by default.' },
    ...history,
  ];
  const changed = new Set();
  const seen = new Set();
  for (let round = 0; round < 8; round++) {
    const message = await anthropicMessage(env, signal, {
      max_tokens: 12_000, system: prompts.coach, messages, tools: coachTools, effort: 'medium',
      output_config: { format: { type: 'json_schema', schema: coachSchema } },
    });
    if (message.stop_reason === 'refusal') return { text: 'I can help with workout plans and sample sessions. Try asking in a different way.', kind: null, changed: [...changed], programs: await store.list() };
    if (message.stop_reason !== 'tool_use') {
      const reply = parseReply(message);
      const programs = await store.list();
      const ids = new Set(programs.map((plan) => plan.id));
      const planId = [reply.program_id, ...Array.from(changed).reverse(), programs.find((plan) => plan.is_master)?.id].find((id) => ids.has(id));
      let kind = null;
      if (reply.kind === 'program' && planId) kind = `program:${planId}`;
      if (['chart', 'chart-all', 'table'].includes(reply.kind)) kind = reply.kind;
      return { text: reply.text || 'Done.', kind, changed: [...changed], programs };
    }

    messages.push({ role: 'assistant', content: message.content });
    const results = [];
    for (const block of message.content.filter((block) => block.type === 'tool_use')) {
      const call = JSON.stringify([block.name, block.input]);
      const result = { type: 'tool_result', tool_use_id: block.id };
      try {
        if (seen.has(call)) throw new AppError(409, 'This action already ran. Use its result above and reply.');
        seen.add(call);
        result.content = JSON.stringify(await runCoachTool(store, env, signal, prompts, block, changed));
      } catch (error) {
        result.is_error = true;
        result.content = error instanceof AppError ? error.message : 'The action could not be completed.';
      }
      results.push(result);
    }
    messages.push({ role: 'user', content: results });
  }
  return { text: 'I reached the limit for this reply. Check your saved programs before trying again.', kind: null, changed: [...changed], programs: await store.list() };
}
