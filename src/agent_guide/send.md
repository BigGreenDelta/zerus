Message another session (hgs guide send)

1. Read the target's identity:
     hgs [@h] inspect SESSION | jq '{run_id,conversation_id,activity,reply_id}'
2. Submit JSON on stdin with a fresh UUID per message:
     jq -n --arg id "$(uuidgen)" --arg run RUN --arg conv CONVERSATION --arg text "..." \
       '{request_id:$id, expected_run_id:$run, expected_conversation_id:$conv, text:$text}' |
       hgs [@h] send SESSION --json
   Reuse a request_id only to retry the identical payload. Optional
   attachments: [{name, mime, data_base64}]; [Image #1] or [File #2] in the
   text places them.
3. "status":"submitted" means the agent received the input, not that it
   replied. An error saying delivery is uncertain means: inspect the session
   and never resend automatically.
4. Read the reply by polling sparingly (for example every 30 seconds):
     hgs [@h] inspect SESSION | jq '{activity,reply_id,last_message}'
   The reply is complete when activity is "idle" and reply_id has changed.
   For a result your own work depends on, declare a wait (hgs guide wait).

Start the text with your name and hgs self reference, for example
"From claude/project/review (zerus://session?...)". The recipient replies
with hgs inspect on that reference and hgs @alias send to your session.
