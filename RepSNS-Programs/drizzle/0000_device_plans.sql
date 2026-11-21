CREATE TABLE device_plans (
  device_id TEXT PRIMARY KEY NOT NULL,
  revision INTEGER NOT NULL DEFAULT 0,
  plans TEXT NOT NULL
);
