-- spido-pg write benchmark schema. Run once before bench.sh.
--
--   psql -U <user> -d <db> -f bench/schema.sql
--
-- The firmware table matches config.json's batch_write resource. Columns
-- are all text to keep the bench load-gen simple — production schemas
-- would type these properly. id is a serial so the writer doesn't have
-- to invent unique values per request.

DROP TABLE IF EXISTS firmware;
CREATE TABLE firmware (
    id         bigserial PRIMARY KEY,
    device_id  text NOT NULL,
    version    text NOT NULL,
    payload    text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now()
);

-- For batch-writer DLQ on retry exhaustion. The writer appends raw text
-- payloads here — we don't index it, just keep it for forensic dump.
DROP TABLE IF EXISTS firmware_dlq;
CREATE TABLE firmware_dlq (
    id      bigserial PRIMARY KEY,
    payload text NOT NULL,
    ts      timestamptz NOT NULL DEFAULT now()
);

-- Used by the cache-invalidation demo (LISTEN/NOTIFY).
-- Triggers on firmware fire NOTIFY so spido_pg's QueryCache wipes
-- "firmware"-tagged entries automatically.
CREATE OR REPLACE FUNCTION notify_firmware_change() RETURNS trigger AS $$
BEGIN
    PERFORM pg_notify('spido_pg_invalidate', 'firmware');
    RETURN NULL;
END
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS firmware_notify ON firmware;
CREATE TRIGGER firmware_notify
    AFTER INSERT OR UPDATE OR DELETE ON firmware
    FOR EACH STATEMENT EXECUTE FUNCTION notify_firmware_change();
