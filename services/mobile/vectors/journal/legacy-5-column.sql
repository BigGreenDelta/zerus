PRAGMA foreign_keys=OFF;
BEGIN TRANSACTION;
CREATE TABLE requests(request_id TEXT PRIMARY KEY,operation TEXT NOT NULL,state TEXT NOT NULL,response TEXT,delivered INTEGER NOT NULL DEFAULT 0);
INSERT INTO requests VALUES('2ce1a28f-d9ec-5557-8033-e54a971047ae','inspect','completed','{"state": "completed", "result": {"text": "legacy content"}, "error": null}',1);
INSERT INTO requests VALUES('3c9dee1f-9fb8-517f-a371-09e0286b5b0f','send','running',NULL,0);
INSERT INTO requests VALUES('83dceee7-a3f4-520b-bc22-3a5583186279','send','completed','{"state": "completed", "result": {"status": "submitted"}, "error": null}',0);
COMMIT;
