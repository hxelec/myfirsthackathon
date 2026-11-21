import { readFileSync } from 'node:fs';
import { DatabaseSync } from 'node:sqlite';

export function localDatabase(file = ':memory:') {
  const db = new DatabaseSync(file);
  db.exec(readFileSync(new URL('../drizzle/0000_device_plans.sql', import.meta.url), 'utf8').replace('CREATE TABLE', 'CREATE TABLE IF NOT EXISTS'));
  return {
    prepare(sql) {
      const statement = db.prepare(sql);
      return {
        bind(...values) {
          return {
            first: async () => statement.get(...values) ?? null,
            run: async () => ({ meta: statement.run(...values) }),
          };
        },
      };
    },
  };
}
