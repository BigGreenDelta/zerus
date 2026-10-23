# Set up speech recognition

In Zerus Voice settings, choose the **processing computer** whose profiles you
want to configure. Run setup commands on that computer, as the same operating
system user that runs Zerus or its connector. Recording still happens on your
phone or desktop; the selected processing computer handles recognition.

Choose the instructions for your provider:

- [Local GigaAM](#local-gigaam): run the model on the processing computer.
- [OpenAI-compatible server](#openai-compatible-server): send audio from that
  computer to an existing transcription API.

Opening help or saving a profile does not install software, download models or
start a service.

## Prepare the processing computer

Both provider types use the small `zerus-speech` service on a supported Unix host,
including Linux and macOS. It needs Python 3.11 or newer. Use Git to obtain the
source if you do not already have a Zerus checkout:

```sh
python3 -c 'import sys; assert sys.version_info >= (3, 11), "Python 3.11 or newer is required"; print(sys.version)'
git --version
```

Use your operating system's package manager to install missing tools. Open a
terminal in an existing reviewed Zerus checkout matching your installed version.
Otherwise, clone the public source into a **new** directory:

```sh
mkdir -p "$HOME/.local/share/zerus-speech"
git clone --depth 1 https://github.com/ufna/zerus.git "$HOME/.local/share/zerus-speech/source" && cd "$HOME/.local/share/zerus-speech/source"
```

Skip the clone when using an existing checkout. The destination must not exist;
these commands do not reset or overwrite an existing checkout. Run the remaining
installation commands from the checkout root in the same terminal.

Create the environment outside the checkout and install the base service:

```sh
python3 -m venv "$HOME/.local/share/zerus-speech/venv"
"$HOME/.local/share/zerus-speech/venv/bin/python" -m pip install --upgrade pip
"$HOME/.local/share/zerus-speech/venv/bin/python" -m pip install ./services/speech
```

The base service uses Python's standard library. An HTTP-only processing computer
needs no GigaAM package, PyTorch, FFmpeg, GPU or model weights.

## Local GigaAM

First [prepare the processing computer](#prepare-the-processing-computer).
GigaAM runs directly through Python and PyTorch. This setup creates a private
Zerus recognition service; it does not install an Ollama model or expose an HTTP
transcription server.

### Install the local runtime

Install FFmpeg with your operating system's package manager and verify it is on
the service's `PATH`. The reviewed GigaAM loader invokes `ffmpeg` even for WAV
recordings:

```sh
ffmpeg -version
```

From the Zerus checkout root, install matching PyTorch and torchaudio versions,
then the pinned GigaAM dependencies. The reviewed pair is 2.10.0:

```sh
"$HOME/.local/share/zerus-speech/venv/bin/python" -m pip install torch==2.10.0 torchaudio==2.10.0
"$HOME/.local/share/zerus-speech/venv/bin/python" -m pip install './services/speech[gigaam]'
```

### Prepare the model and device

Download and load Russian GigaAM v3 E2E RNNT, including its tokenizer, on CPU:

```sh
"$HOME/.local/share/zerus-speech/venv/bin/python" -c 'import gigaam; gigaam.load_model("v3_e2e_rnnt", device="cpu", use_flash=False)'
```

Weights are cached in this user's `~/.cache/gigaam`. CPU is the baseline and
requires no GPU. To use another supported model, replace the model name in this
command with the exact name selected in Zerus and prepare its weights first.

For an NVIDIA GPU, check the installed environment:

```sh
"$HOME/.local/share/zerus-speech/venv/bin/python" -c 'import torch; print("CUDA available:", torch.cuda.is_available())'
```

CUDA requires a compatible NVIDIA driver and PyTorch build. Use the
[official PyTorch installation selector](https://pytorch.org/get-started/locally/)
when preparing a different build, keeping PyTorch and torchaudio versions matched.
Choose `cuda`, `cuda:0` or `cuda:1` explicitly for the intended GPU. Apple `mps`
also requires a compatible GPU/runtime and a successful model Test. Zerus does
not automatically switch devices; use `cpu` when a GPU has not been verified.

### Configure and test

[Start the speech service](#start-the-speech-service), then return to Voice
settings on the same processing computer. Add a profile with:

| Field | CPU baseline |
| --- | --- |
| Type | Local GigaAM |
| Model | `v3_e2e_rnnt` |
| Language | `ru` |
| Device | `cpu` |

Save the profile and run **Test**. Test loads the saved model/device using a short
synthetic sound. A responding service alone does not prove that inference works.
After success, explicitly select the profile for new recordings. Saved previews
keep their original provider selection until you explicitly change it.

## OpenAI-compatible server

To run your own GigaAM endpoint on a separate computer, follow
[Standalone speech HTTP server](speech-server.md). The steps below configure
the processing computer as a client of that server.

Use this provider when you already have a server implementing the transcription
API below. The server performs model inference; the processing computer only
forwards the audio and receives the transcript.

First [prepare the processing computer](#prepare-the-processing-computer) with
the base `./services/speech` package, then [start its speech
service](#start-the-speech-service). The local GigaAM installation and model
preparation steps are unnecessary for this provider.

### Check the server's API

The server must accept:

- `POST {base_url}/audio/transcriptions`.
- Multipart `file`: a mono 16 kHz PCM WAV file named `audio.wav`.
- Multipart `model`: the model identifier configured on that server.
- Optional multipart `language`: a two-letter lowercase code, such as `ru`.
- Multipart `response_format=json`.
- Optional `Authorization: Bearer ...` when an API key is configured.

It must return HTTP 200 with a JSON object containing a string `text`, for example
`{"text":"Recognized words"}`. Zerus accepts a response up to 64 KiB and a
transcript up to 32,000 UTF-8 bytes, with a 60-second recognition deadline. It does
not follow redirects or automatically retry a recognition POST.

A chat-completions endpoint or an Ollama model name alone does not establish
transcription compatibility. Check the server's documented audio API; an arbitrary
GigaAM URL is insufficient. This adapter uses the compatible protocol without
requiring an OpenAI account or the OpenAI SDK.

### Configure and test

In Voice settings, select the processing computer and add a profile:

| Field | Meaning |
| --- | --- |
| Type | OpenAI-compatible server |
| Base URL | API prefix, for example `https://speech.example.net/v1`; Zerus appends `/audio/transcriptions` |
| Model | The server's actual transcription model identifier |
| Language | `ru`, another supported two-letter code, or empty to omit the hint |
| API key | Optional key issued by that server; choose Replace to set it, Keep to retain it or Remove to clear it |

The URL must be reachable **from the processing computer**. `localhost` refers
to that computer, not your phone. Put the key in the API key field, not the URL;
base URLs cannot contain credentials, query strings or fragments. Saved keys are
kept on the processing computer and are never returned to clients.

Use HTTPS with a valid certificate. **Allow HTTP** is an explicit option for a
trusted local/private server: it sends audio and the key without encryption and
is accepted only for an actual private or loopback destination. Ambient proxy
settings are ignored.

Save, then run **Test** on the saved profile. It sends a short synthetic sound to
the configured API; it uses no microphone recording and sends nothing to an agent.
After success, explicitly choose the profile for new recordings. Saving or editing
a profile does not retarget an existing recorded preview.

## Start the speech service

Use the same user account and, if customized, the same `HGS_CONFIG_DIR` as the
processing computer's Zerus CLI or connector. The default is `~/.config/hgs`.
Profiles saved in Zerus need no `speech.json` file. Preserve any existing
configuration.

Run in the foreground on Linux or macOS:

```sh
"$HOME/.local/share/zerus-speech/venv/bin/python" -m zerus_speech
```

Keep that terminal open. Ctrl+C stops the speech service. Run only one instance
for this user.

On Linux with systemd user services, stop the foreground instance first. From
the same Zerus checkout, explicitly enable the persistent service:

```sh
python3 services/speech/install-user-service.py --python "$HOME/.local/share/zerus-speech/venv/bin/python" --start
systemctl --user status zerus-speech.service --no-pager
```

This starts only `zerus-speech.service`. On macOS, use the foreground command or
your own service supervisor. The Linux installer accepts an absent default
`speech.json`; an explicitly supplied `--config` must exist.

Refresh provider availability in Zerus after the service is ready. For Linux
startup or recognition errors, inspect:

```sh
journalctl --user -u zerus-speech.service -n 50 --no-pager
```

Keep the original recording if a step fails. Read the command output on the
processing computer and correct that profile's setup before trying again. For
advanced service paths and limits, see the [speech service README](../services/speech/README.md).
