export class AppError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

export function templatesFor(assets) {
  return [
    { id: 'repsns-master', name: 'RepSNS Master Plan', program: assets.master },
    { id: 'wheelchair-upper-body', name: 'Seated upper-body plan (wheelchair user)', program: assets.seated },
  ];
}

const timestamp = () => new Date().toISOString().slice(0, 16).replace('T', ' ');

function makeItem(name, program, answers, template) {
  const date = timestamp();
  return {
    id: crypto.randomUUID(), name, created: date, updated: date, is_master: false,
    answers: answers ?? {}, program, ...(template ? { template } : {}),
  };
}

function templateItem(template) {
  return makeItem(template.name, template.program, { notes: `Pre-made plan: ${template.name}.` }, template.id);
}

export function getPlan(plans, id) {
  const item = plans.find((plan) => plan.id === id);
  if (!item) throw new AppError(404, 'That program no longer exists.');
  return item;
}

function makeMaster(plans, item) {
  for (const plan of plans) plan.is_master = plan === item;
}

export function planSummary(item) {
  return {
    id: item.id, name: item.name, is_master: item.is_master, updated: item.updated, title: item.program.title,
    days: item.program.days.map((day) => `${day.name} (${day.estimated_minutes} min, ${day.exercises.length} exercises)`),
  };
}

export class PlanStore {
  constructor(db, device, templates) {
    this.db = db;
    this.device = device;
    this.templates = templates;
  }

  async read() {
    const query = this.db.prepare('SELECT revision, plans FROM device_plans WHERE device_id = ?').bind(this.device);
    let row = await query.first();
    if (!row) {
      const plans = this.templates.map(templateItem);
      makeMaster(plans, plans[0]);
      await this.db.prepare('INSERT OR IGNORE INTO device_plans (device_id, plans) VALUES (?, ?)')
        .bind(this.device, JSON.stringify(plans)).run();
      row = await query.first();
    }
    return { revision: row.revision, plans: JSON.parse(row.plans) };
  }

  async list() {
    return (await this.read()).plans;
  }

  async change(data, { reuseMissingId = false } = {}) {
    for (let attempt = 0; attempt < 5; attempt++) {
      const { plans, revision } = await this.read();
      const item = this.apply(plans, data, reuseMissingId);
      const serialized = JSON.stringify(plans);
      if (new TextEncoder().encode(serialized).byteLength > 1_800_000) {
        throw new AppError(409, 'Your plan library is full. Delete an unused plan before saving another.');
      }
      const result = await this.db.prepare('UPDATE device_plans SET plans = ?, revision = revision + 1 WHERE device_id = ? AND revision = ?')
        .bind(serialized, this.device, revision).run();
      if (result.meta.changes === 1) return { item, programs: plans };
    }
    throw new AppError(409, 'Your plans changed in another request. Try again.');
  }

  apply(plans, data, reuseMissingId) {
    if (data.action === 'load_template') {
      const template = this.templates.find((candidate) => candidate.id === data.template_id);
      if (!template) throw new AppError(404, 'That template is unavailable.');
      const existing = plans.find((plan) => plan.template === template.id);
      if (existing) return existing;
      const item = templateItem(template);
      plans.push(item);
      return item;
    }

    if (data.action === 'save') {
      const name = String(data.name || data.program.title || 'My program').trim().slice(0, 120) || 'My program';
      let item = data.id ? plans.find((plan) => plan.id === data.id) : null;
      if (data.id && !item && !reuseMissingId) throw new AppError(404, 'That program no longer exists.');
      if (item) {
        Object.assign(item, { name, program: data.program, updated: timestamp() });
        if (data.answers && Object.keys(data.answers).length) item.answers = data.answers;
      } else {
        item = makeItem(name, data.program, data.answers);
        plans.push(item);
      }
      if (data.master) makeMaster(plans, item);
      return item;
    }

    if (!['rename', 'master', 'delete'].includes(data.action)) throw new AppError(400, 'Could not save that change.');
    const item = getPlan(plans, data.id);
    if (data.action === 'rename') {
      item.name = String(data.name ?? '').trim().slice(0, 120) || item.name;
      item.updated = timestamp();
    }
    if (data.action === 'master') makeMaster(plans, item);
    if (data.action === 'delete') {
      if (plans.length === 1) throw new AppError(400, 'You need at least one plan. Build or save another plan before deleting this one.');
      plans.splice(plans.indexOf(item), 1);
      if (item.is_master) makeMaster(plans, plans.at(-1));
    }
    return item;
  }
}
