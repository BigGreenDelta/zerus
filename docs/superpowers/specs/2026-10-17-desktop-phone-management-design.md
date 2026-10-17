# Desktop phone pairing and management

## Intent

The owner could not find where the desktop shows a pairing QR code. It never
did: a phone invitation exists only as the relay administrator's `invite`
command on the relay host. The owner asked for an explicit **Pair a phone**
action in Settings → Mobile connection and in Machines, and for Machines to list
the linked phones: "I want to see where I gave permission to control, revoke if
needed". Owner decision (2026-10-17), after the trust trade-off below was
presented: "да, как ты предложил. менеджмент со всех компьютеров по графу" —
every computer manages phones, through the machine graph.

## Goals

- Any computer can show a single-use pairing QR code for a workspace it reaches,
  either as a gateway (its own connector credential) or through one enabled
  direct peer that is a gateway.
- Machines lists every phone with access to each reachable workspace: name,
  platform, when it was paired, when it was last active and which computer added
  it. A confirmed **Revoke** removes that phone's credential, queued requests and
  push registration.
- Phone apps need no change: they already scan `zerus://pair?server=…&code=…`.

## Non-goals

- Per-phone or per-computer grants. Device credentials stay workspace-scoped.
- Renaming phones, phone-side lists of other phones, or a relay web console.
- Removing the administrator `invite` and `revoke-device` commands.

## Trust model

A gateway credential may now create phone invitations and revoke phones of its
own workspace. A phone reaches every computer of the workspace, so pairing
through one gateway grants access to the others; the owner accepted this
because every computer lists the phones, shows who added each one and can
revoke any of them. Invitations keep their contract: ten minutes, single use,
hashed at rest, rate limited. Peers never receive gateway credentials: a peer
asks its gateway over the existing SSH edge, one hop, exactly as phones reach
peers through gateways.

## Relay (API v1, additive)

| Method and path | Role | Result |
| --- | --- | --- |
| `POST /v1/node/invitations` | Computer | `{workspace_id?}` → `{pair_code, expires_at, workspace_id, workspace_name}`; 409 for another workspace |
| `GET /v1/node/devices` | Computer | `{workspace_id, workspace_name, devices: [{id, name, platform, paired_at, last_seen_at, added_by}]}` |
| `DELETE /v1/node/devices/{id}?workspace_id=` | Computer | `{revoked: true}`; 404 outside the workspace or already revoked, 409 for another named workspace |

- `devices` gains nullable `paired_at`, `last_seen` and `added_by` (node ID);
  `invitations` gains nullable `added_by`. Older rows report `null`.
- `platform` is `ios` for an `apns`/`apns_sandbox` push registration, `android`
  for `fcm`, otherwise `null`. `added_by` is `{computer_id, name}` of the issuing
  gateway or `null` (administrator command, older pairing).
- Device authentication refreshes `last_seen` at most every five minutes, so the
  long-poll path does not write on every request.
- The list is bounded (200 newest active devices, ordered by pairing time).
- Invitation creation shares the pairing rate limiter keyed by gateway.
- Revocation reuses the administrator path: credential, queued requests and push
  registration, then wakes the workspace.

## `hgs mobile`

`hgs [@host] mobile phones|invite|revoke-phone ID --json` runs on the computer
that holds the connector configuration (`<config>/mobile/connector.json`:
`server_url`, `node_token`). It works without local sessions and is forwarded to
a peer like other read-only machine commands.

- `phones` without a configuration prints `{"configured": false}` and succeeds,
  so callers can probe every machine. Otherwise it prints the relay response plus
  `server_url` and `gateway` (this computer's name).
- `invite` adds `pair_url`: `zerus://pair?server=<form-encoded server_url>&code=<code>`.
- HTTPS goes through the system `curl`, started with `-q` first so no `.curlrc`
  can add redirects, traces or other defaults. The bearer token travels on curl's
  standard input configuration, never in arguments; proxies from the
  environment are ignored like in the connector; only `https` is allowed, except
  loopback `http` when `HGS_MOBILE_ALLOW_INSECURE_LOCALHOST=1` (tests). Bounded
  connect/total timeouts and response size.
- `invite` and `revoke-phone` take `--workspace ID --server URL` from the list the
  desktop shows. hgs refuses a changed relay before any request and sends the
  workspace to the relay, which answers 409 for another workspace before creating
  or revoking anything; the desktop also checks the returned workspace and relay
  before it shows a code. (Independent review of PR #192, zerus-735w.1.)
- A relay without the routes (404) reports "The relay does not support phone
  management yet; update it."

## Desktop

- **Pair a phone dialog.** Shows the workspace name, a QR code of `pair_url`, the
  server and the code as selectable text with Copy, and a countdown. **New code**
  requests another invitation; closing discards it. With several reachable
  workspaces the dialog first asks which one.
- **Machines → Phones.** A section listing phones grouped by workspace, with
  "via <computer>" when reached through a peer, **Pair a phone…**, **Refresh**
  and per-phone **Revoke…** with a destructive confirmation naming the phone. A
  failed or unconfirmed revocation stays reported until **Refresh** and is never
  retried automatically.
  Machines without a connector are skipped; unreachable or outdated peers are
  reported in one line.
- **Settings → Mobile connection** gains **Pair a phone…**, opening the same
  dialog.
- QR encoding uses the vendored Nayuki QR Code generator (MIT), with its license
  in the repository.

## Compatibility and rollout

The phone API, admin CLI and connector are unchanged. Older relays and peers fail
closed with an explicit message. The relay must be deployed before the desktop
feature works on the production relay; that rollout is owner-authorized.
