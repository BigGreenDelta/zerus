Wait for another session's result (hgs guide wait)

Declare a wait when your work depends on a result from an independent
session. Prose, an idle agent or a native Stop never completes one. Requests
need a lowercase canonical UUID: uuidgen | tr A-F a-f.

Waiter, before sending the assignment:
  echo '{"request_id":"UUID","producer_reference":"zerus://...","label":"Review the patch"}' |
    hgs wait begin --json
  The producer reference comes from hgs [@h] reference SESSION --json | jq -r .uri
  Keep request_id to retry the same declaration. Send the producer the
  response's reply_ticket, your host and your session name with the task.

Producer, after receiving a ticket:
  acknowledge:  ticket fields + new request_id | hgs @WAITER_HOST wait ack --json
  deliver the result with hgs send to the waiter's session (hgs guide send),
  then finish:  ticket fields + new request_id + result_request_id (that
                send's request_id) | hgs @WAITER_HOST wait finish --json

Cancel an abandoned wait: hgs wait cancel --json with request_id, wait_id,
waiter_machine, expected_run_id and expected_conversation_id.
List open and settled waits: hgs wait list --json with expected_run_id and
expected_conversation_id; pass next_cursor as before for older entries.
