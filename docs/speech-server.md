# Standalone speech HTTP server

`zerus-speech-http` serves local GigaAM models through an OpenAI-compatible
transcription API. It is an optional package in the Zerus repository and can run
on a dedicated Linux or macOS computer without the desktop application, `hgs`,
agent sessions, the mobile relay or a Zerus account. Clients choose its API URL,
model and key through their ordinary HTTP provider settings.

## Install and prepare

Use Python 3.11+ and a reviewed Zerus source checkout. From the checkout root:

```sh
python3 -m venv "$HOME/speech-venv"
"$HOME/speech-venv/bin/pip" install './services/speech[http,gigaam]'
```

Install FFmpeg with the operating system's package manager. Install the matching
PyTorch/torchaudio build and explicitly prepare the desired weights as described
in [Local GigaAM](speech-setup.md#local-gigaam). The baseline is CPU; CUDA requires
a supported NVIDIA driver and a CUDA-enabled PyTorch build. The HTTP package
alone (`[http]`) does not install GigaAM, Torch or model weights.

Create a private configuration directory and a key file:

```sh
install -d -m 700 "$HOME/.config/zerus-speech-server"
python3 - <<'PY'
import os
from pathlib import Path
import secrets
path = Path.home() / '.config/zerus-speech-server/api-key'
with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as output:
    output.write(secrets.token_urlsafe(32) + '\n')
PY
```

Keep an existing key rather than overwriting it. Put this configuration in
`~/.config/zerus-speech-server/server.json`, substituting an absolute private
`state_dir` owned by the service user. Use `cpu` until CUDA is verified:

```json
{
  "schema": 1,
  "state_dir": "/home/user/.local/state/zerus-speech-server",
  "profiles": [
    {
      "id": "local-ru",
      "name": "Russian GigaAM",
      "adapter": "gigaam",
      "model": "v3_e2e_rnnt",
      "language": "ru",
      "device": "cpu"
    }
  ]
}
```

Run the independently installed service:

```sh
"$HOME/speech-venv/bin/zerus-speech-http" \
  --config "$HOME/.config/zerus-speech-server/server.json" \
  --api-key-file "$HOME/.config/zerus-speech-server/api-key" \
  --model gigaam-ru=local-ru
```

This owns both the inference queue and its private Unix socket. The default HTTP
listener is `127.0.0.1:8765`. Stop with Ctrl+C; use a supervisor for persistence.
On Linux a user unit can run that exact command with absolute paths,
`Restart=on-failure`, `UMask=0077` and `TimeoutStopSec=30`; enable only that new
unit. Bind to the actual private/VPN IP with `--host` and choose a port with
`--port`. The key is mandatory, including on loopback. Its file must be a private
regular file owned by the service user. Never put it in a URL or a unit command.

For ordinary networks, terminate HTTPS at a reverse proxy on the server. Limit
its request body to 26 MiB, forward Authorization and allow at least 60 seconds
for recognition. Within an encrypted private VPN, HTTP can be used with an
explicit private-network opt-in on the client. Bind and firewall the listener
to the intended VPN computers; DNS naming alone does not restrict access.
The API has no browser UI or CORS configuration requirement for native clients.

## Models and compatibility

Supported routes:

| Route | Contract |
| --- | --- |
| `GET /v1/models` | OpenAI model list: `object: list`, `data` entries with `id`, `object`, `created`, `owned_by`. Only explicitly exposed, available local profiles are listed. |
| `POST /v1/audio/transcriptions` | Multipart `file` and `model`, optional `language`, `response_format=json` or `text`, `stream=false`, `temperature=0`. JSON returns `{"text":"..."}`. |

Both routes require `Authorization: Bearer <key>`. This is the compatible
[transcription API shape](https://developers.openai.com/api/reference/resources/audio/subresources/transcriptions/methods/create),
not the complete OpenAI API. Unsupported parameters and response formats are
rejected instead of silently ignored; streaming, timestamps, prompt conditioning,
diarization, translation and speech generation are not implemented.

The public model ID selects the exact local profile. Repeat
`--model PUBLIC_ID=LOCAL_PROFILE_ID` for additional models; duplicate or unknown
public IDs are not aliases for a default model. An unknown requested model returns
404. `language` must be a lowercase two-letter code supported by the chosen
profile; multilingual profiles use an empty configured language and recognize
Russian, English, Kazakh, Kyrgyz and Uzbek. The hint does not translate speech.
Only local GigaAM profiles can be exposed; HTTP-forwarding profiles are excluded.
The underlying service keeps one resident model at a time, so changing models
can add model-loading latency. HTTP never downloads missing weights.

Uploads are at most 25 MiB; normalized audio is mono 16 kHz PCM, at most 120
seconds. Canonical PCM WAV needs no decoder process. Other supported FFmpeg
containers are decoded in a bounded child using pipe-only input: client filenames
are ignored and playlists cannot fetch network URLs or local files. Decoding is
limited to 15 seconds. Four admitted HTTP requests are allowed; the existing
bounded inference queue remains authoritative. Overload returns 429 with
`Retry-After`; recognition has a 55-second work deadline followed by bounded
local job cancellation. Errors have `error: {message,type,param,code}` and contain
no credentials, audio, transcripts, internal paths or raw provider exceptions.

Disconnect, deadline or HTTP shutdown cancels that request's exact accepted job.
An in-progress GPU call may finish before cancellation takes effect; its result
is not published as a successful response. Private receipts and result retention
follow the [speech service contract](../services/speech/README.md#json-interface).
HTTP access logging is disabled. Every POST creates one private request UUID;
the HTTP interface provides no cross-request idempotency guarantee. Clients must
disable automatic transcription retries after uncertain delivery (including
SDK retry defaults). The Zerus HTTP adapter already does so.

## Attach to an existing speech service

To expose the same prepared models and queue to other computers, install only
`[http]` and run the gateway as the same Unix user:

```sh
"$HOME/speech-venv/bin/zerus-speech-http" \
  --socket "$HOME/.local/state/hgs/speech/service.sock" \
  --api-key-file "$HOME/.config/zerus-speech-server/api-key" \
  --model gigaam-ru=gigaam-ru
```

The socket and its parent must be private and owned by that user. This mode
does not start another inference service or load another model. Existing local
profiles and GUI operation continue using their original socket.

Do not configure an HTTP profile that calls this gateway in the **same backing
speech queue**: its single worker would wait for inference queued behind itself.
For a server and client on the same computer, use standalone mode with its own
state directory and queue. That creates an independent resident model and needs
corresponding GPU/RAM capacity. Prepared weights can share the existing cache.
For a separate server there is no such queue cycle.

## Connect Zerus and verify

On the processing computer, keep the lightweight private speech service and add
an **OpenAI-compatible server** profile. Set the base URL including `/v1`, the
public model ID and the server key. An example is
`https://speech.example.test/v1`, model `gigaam-ru`, language `ru`. Run **Test**,
then select it explicitly for new recordings. A saved preview retains its
original provider pin. Mobile requests keep their existing phone → relay → PC
route; only the PC needs access to this server. Other clients can use the same
standard audio API without installing Zerus.

From a source checkout, isolated HTTP/socket/model-selection/authentication/
cancellation and standalone-CLI contracts run with:

```sh
"$HOME/speech-venv/bin/python" -m unittest discover -s services/speech/tests -v
```

These use synthetic profiles and private state. Real model recognition must also
be qualified on the intended server, including recordings near its duration limit.
