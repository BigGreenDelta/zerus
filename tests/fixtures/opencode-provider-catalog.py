#!/usr/bin/env python3
"""Regenerates the OpenCode providers phase 1 contract fixture.

Run from the repository root: python3 -I tests/fixtures/opencode-provider-catalog.py
It writes tests/fixtures/opencode-provider-catalog.json and the byte-identical Android
test copy (which the iOS tests read). With one argument it writes only that file.

The expected values come from this reference model of the phase 1 rules in
docs/superpowers/plans/2026-10-18-opencode-providers-phase-1.md; hgs, the Rust
connector, the desktop, Android and iOS must agree with it. Change a rule here and in
every implementation together."""
import json
import sys
from pathlib import Path

SECRET = 'fixture-secret-key'
HOME = '/home/example'
ZERO = [{'input': 0, 'output': 0, 'cache': {'read': 0, 'write': 0}}]
HIDDEN = {'settings': {'apiKey': SECRET, 'baseURL': 'http://127.0.0.1:9/v1'},
          'headers': {'Authorization': 'Bearer ' + SECRET}, 'body': {'token': SECRET}}


def paid(i, o):
    return [{'input': i, 'output': o, 'cache': {'read': 0, 'write': 0}}]


def model(provider, mid, name, cost, variants=(), enabled=True, hidden=False):
    m = {'id': mid, 'modelID': mid, 'providerID': provider, 'name': name,
         'capabilities': {'tools': True, 'input': ['text'], 'output': ['text']},
         'variants': [{'id': v, 'settings': {'reasoningEffort': v}} for v in variants],
         'time': {'released': 1790000000000}, 'cost': cost, 'status': 'active', 'enabled': enabled,
         'limit': {'context': 131072, 'output': 8192}}
    if hidden:
        m.update(HIDDEN)
    return m


MODELS = [
    model('opencode', 'glm-5.3-flash-free', 'GLM-5.3-Flash Free', ZERO, ['low', 'high', 'max']),
    model('opencode', 'big-pickle', 'Big Pickle', ZERO),
    model('groq', 'openai/gpt-oss-20b', 'GPT OSS 20B', paid(0.075, 0.3), ['low', 'medium', 'high']),
    model('groq', 'groq/compound', 'Compound', ZERO),
    model('fixture', 'local', 'Native fixture', [], ['low', 'medium', 'high'], hidden=True),
    model('fixture', 'alternate', 'Alternate fixture', [], hidden=True),
    model('ollama', 'qwen3-coder:30b', 'qwen3-coder:30b', []),
    model('keyed', 'paid-model', 'Paid model', paid(3, 15)),
    model('keyed', 'off', 'Disabled model', paid(1, 1), enabled=False),
    model('solo', 'solo-model', 'Solo model', paid(1, 2)),
]
PROVIDERS = [
    {'id': 'opencode', 'integrationID': 'opencode', 'name': 'OpenCode', 'activation': 'enabled',
     'package': '@opencode/ai/providers/openai-compatible',
     'settings': {'apiKey': 'public', 'baseURL': 'https://opencode.example.invalid/zen/v1'}},
    {'id': 'groq', 'integrationID': 'groq', 'name': 'Groq', 'activation': 'auto', 'package': '@opencode/ai/providers/groq'},
    {'id': 'fixture', 'name': 'Private keyless fixture', 'activation': 'enabled',
     'package': '@opencode/ai/providers/openai-compatible', **HIDDEN},
    {'id': 'ollama', 'name': 'Ollama', 'activation': 'enabled', 'package': '@opencode/ai/providers/openai-compatible',
     'settings': {'baseURL': 'http://127.0.0.1:11434/v1', 'provider': 'ollama'}},
    {'id': 'keyed', 'integrationID': 'keyed', 'name': 'Keyed fixture', 'activation': 'enabled',
     'package': '@opencode/ai/providers/openai-compatible'},
    {'id': 'solo', 'integrationID': 'solo', 'name': 'Solo fixture', 'activation': 'enabled',
     'package': '@opencode/ai/providers/openai-compatible'},
]
INTEGRATIONS = [
    {'id': 'anthropic', 'name': 'Anthropic', 'methods': [{'type': 'key'}, {'type': 'env', 'names': ['ANTHROPIC_API_KEY']}],
     'connections': []},
    {'id': 'opencode', 'name': 'OpenCode Console', 'methods': [{'type': 'key', 'label': 'API key (service account)'}],
     'connections': []},
    {'id': 'groq', 'name': 'Groq', 'methods': [{'type': 'key'}, {'type': 'env', 'names': ['GROQ_API_KEY']}],
     'connections': [{'type': 'env', 'name': 'GROQ_API_KEY'}]},
    {'id': 'fixture', 'name': 'Private keyless fixture', 'methods': [{'type': 'key', 'label': 'Manually enter API Key'}],
     'connections': []},
    {'id': 'keyed', 'name': 'Keyed fixture', 'methods': [{'type': 'key'}], 'connections': [
        {'type': 'credential', 'id': 'cred_fixture_team', 'label': 'Team', 'method': 'key'},
        {'type': 'credential', 'id': 'cred_fixture_personal', 'label': 'Personal', 'method': 'key'}]},
    {'id': 'solo', 'name': 'Solo fixture', 'methods': [{'id': 'device', 'type': 'oauth', 'label': 'Sign in'}], 'connections': [
        {'type': 'credential', 'id': 'cred_fixture_work', 'label': 'Work', 'method': 'oauth',
         'status': {'status': 'needs_auth', 'message': 'Sign in again with ' + SECRET,
                    'url': 'https://auth.example.invalid/device?code=' + SECRET}}]},
]
DEFAULT = model('fixture', 'local', 'Native fixture', [], ['low', 'medium', 'high'], hidden=True)
FAVORITES = ['keyed/paid-model', 'groq/openai/gpt-oss-20b', 'gone/missing']
CATALOG_AT = 1792267200.5
# A project folder whose opencode.json declares one more provider: OpenCode lists it,
# with an integration keyed by its ID, only for that location.
FOLDER = HOME + '/project'
FOLDER_PROVIDER = {'id': 'workspace', 'name': 'Workspace models', 'activation': 'enabled',
                   'package': '@opencode/ai/providers/openai-compatible', **HIDDEN}
FOLDER_MODEL = model('workspace', 'team-coder', 'Team coder', [], ['low', 'high'], hidden=True)
FOLDER_INTEGRATION = {'id': 'workspace', 'name': 'Workspace models',
                      'methods': [{'type': 'key', 'label': 'Manually enter API Key'}], 'connections': []}


def free(cost):
    return isinstance(cost, list) and bool(cost) and all(c.get('input') == 0 and c.get('output') == 0 for c in cost)


def catalog(provider_list, model_list, integration_list, default):
    """The full catalog, the short list and the provider rows of one location."""
    labels = {p['id']: p['name'] for p in provider_list}
    full = []
    for m in model_list:
        if m['enabled'] is False:
            continue
        value = m['providerID'] + '/' + m['id']
        efforts = [v['id'] for v in m['variants']]
        full.append({'id': value, 'value': value, 'label': m['name'], 'provider': m['providerID'],
                     'provider_label': labels.get(m['providerID'], m['providerID']), 'free': free(m['cost']),
                     'effort_options': efforts, 'effort_labels': efforts})
    by_value = {e['value']: e for e in full}
    configured = {p['id'] for p in provider_list if not p.get('integrationID')}
    short = []
    candidates = ([by_value[default]] + [by_value[f] for f in FAVORITES if f in by_value]
                  + [e for e in full if e['provider'] in configured] + [e for e in full if e['free']])
    for entry in candidates:
        if all(e['value'] != entry['value'] for e in short):
            short.append(dict(entry))
    for entry in short:
        if entry['value'] in FAVORITES:
            entry['favorite'] = True
        if entry['value'] == default:
            entry['default'] = True
    integrations = {i['id']: i for i in integration_list}
    providers = []
    for p in provider_list:
        integration = integrations.get(p.get('integrationID') or p['id'])
        connections = (integration or {}).get('connections', [])
        credentials = [c for c in connections if c.get('type') == 'credential']
        own = [e for e in full if e['provider'] == p['id']]
        if credentials:
            kind = {'oauth': 'oauth', 'external': 'external', 'command': 'command'}.get(credentials[0]['method'], 'key')
        elif any(c.get('type') == 'env' for c in connections):
            kind = 'env'
        elif not p.get('integrationID'):
            kind = 'local' if integration is None else 'config'
        elif own and all(e['free'] for e in own):
            kind = 'free'
        else:
            kind = 'config'
        row = {'id': p['id'], 'label': p['name'], 'kind': kind, 'credential_count': len(credentials), 'model_count': len(own)}
        if len(credentials) == 1:
            row['active_label'] = credentials[0]['label']
        providers.append(row)
    return full, short, providers


def account_models(default):
    """`hgs account models native-opencode --location FOLDER --json` with the service
    answering this folder: the folder's row keys, every model with its marks, the stored
    favorites with whether the folder lists them, and its own providers marked."""
    full, short, providers = catalog(PROVIDERS + [FOLDER_PROVIDER], MODELS + [FOLDER_MODEL],
                                     INTEGRATIONS + [FOLDER_INTEGRATION], default)
    home = {p['id'] for p in PROVIDERS}
    for row in providers:
        if row['id'] not in home:
            row['folder'] = True
    models = []
    for entry in full:
        entry = dict(entry)
        if entry['value'] in FAVORITES:
            entry['favorite'] = True
        if entry['value'] == default:
            entry['default'] = True
        models.append(entry)
    values = {e['value'] for e in full}
    answer = {'model_options': short, 'model_options_truncated': False, 'providers': providers,
              'providers_truncated': False, 'settings_model': default, 'location': FOLDER,
              'favorites': [{'value': f, 'available': f in values} for f in FAVORITES],
              'models_truncated': False, 'models': models, 'model_catalog_source': 'opencode_service',
              'catalog_state': 'ready', 'catalog_reason': '', 'catalog_at': CATALOG_AT}
    at = {'location': {'directory': FOLDER}}
    return {
        'description': ('hgs account models for a project folder. service holds the raw answers for that '
                        'location (the home answers are the top-level service: workspace is listed only '
                        'here); answer is the complete hgs output with catalog_at fixed.'),
        'location': FOLDER,
        'service': {'location': FOLDER, 'provider': {**at, 'data': PROVIDERS + [FOLDER_PROVIDER]},
                    'model': {**at, 'data': MODELS + [FOLDER_MODEL]}, 'default': {**at, 'data': DEFAULT},
                    'integration': {**at, 'data': INTEGRATIONS + [FOLDER_INTEGRATION]}},
        'answer': answer,
    }


def build():
    default = DEFAULT['providerID'] + '/' + DEFAULT['id']
    full, short, providers = catalog(PROVIDERS, MODELS, INTEGRATIONS, default)
    row = {'settings_model': default, 'model_options': short, 'providers': providers,
           'model_options_truncated': False, 'providers_truncated': False}

    def option(e):
        o = {'value': e['value'], 'label': e['label'], 'provider': e['provider'], 'provider_label': e['provider_label'],
             'free': e['free']}
        if e.get('favorite'):
            o['favorite'] = True
        if e.get('default'):
            o['default'] = True
        if e['effort_options']:
            o['effort_options'] = e['effort_options'][:8]
            o['effort_labels'] = e['effort_labels'][:8]
        return o

    base = {'id': 'native-opencode', 'provider': 'opencode', 'label': 'Native OpenCode configuration',
            'native': True, 'installed': True, 'is_default': True}
    usage = {'status': 'unavailable', 'identity': {}, 'windows': [], 'refreshing': False, 'refresh_error': False}
    options = [option(e) for e in short]
    hgs_account = {**base, 'catalog_state': 'ready', 'catalog_reason': '', 'catalog_at': CATALOG_AT, **row}
    heartbeat = {**base, 'model_options': options, 'catalog_state': 'ready', 'catalog_at': CATALOG_AT,
                 'model_options_truncated': False, 'providers': providers, 'providers_truncated': False,
                 'usage': {**usage, 'stale': True}}
    launch = {'id': base['id'], 'provider': base['provider'], 'label': base['label'], 'native': True, 'is_default': True,
              'installed': True, 'auth_status': 'unknown', 'model_options': options, 'catalog_state': 'ready',
              'catalog_at': CATALOG_AT, 'model_options_truncated': False}
    stopped = {**base, 'model_options': options[:1], 'catalog_state': 'not_running',
               'catalog_reason': 'OpenCode is not running on fixture', 'catalog_at': CATALOG_AT,
               'model_options_truncated': True, 'providers': providers[:2], 'providers_truncated': True,
               'usage': {**usage, 'stale': True}}
    # The hgs row that the connector projects as stopped_account.
    stopped_hgs = {**base, 'catalog_state': 'not_running', 'catalog_reason': 'OpenCode is not running on fixture',
                   'catalog_at': CATALOG_AT, 'settings_model': default, 'model_options': short[:1],
                   'providers': providers[:2], 'model_options_truncated': True, 'providers_truncated': True}
    lengths = [{'value': v} for v in (
        {'a': 'x'},
        ['café', '\U0001F600', '\u0000\u001f', '"\\', '\u007f', '\n\t\r\b\f', 1, 1.5, True, None, [], {}],
        {'label': '界' * 3, 'nested': {'k': [0, -2, CATALOG_AT]}},
        hgs_account,
        heartbeat,
    )]
    for case in lengths:
        case['length'] = len(json.dumps(case['value'], ensure_ascii=True))
    return {
        'version': 1,
        'description': ('OpenCode providers phase 1 contract shared by hgs, the Rust connector, the desktop, Android '
                        'and iOS; regenerate with tests/fixtures/opencode-provider-catalog.py. service holds raw '
                        'OpenCode 2.0.32-shaped responses that deliberately carry secrets; favorites is the stored '
                        'favorites list; row is what hgs adds to the native-opencode account row for the home '
                        'location; hgs_account is that complete row as the connector reads it; heartbeat_account '
                        'and launch_account are the connector projections of hgs_account; stopped_account is a '
                        'truncated row of a stopped service; ascii_lengths pins len(json.dumps(value, '
                        'ensure_ascii=True)), the measure of the 32 KiB row budget and the 128 KiB projection.'),
        'service': {'location': HOME, 'provider': {'location': {'directory': HOME}, 'data': PROVIDERS},
                    'model': {'location': {'directory': HOME}, 'data': MODELS},
                    'default': {'location': {'directory': HOME}, 'data': DEFAULT},
                    'integration': {'location': {'directory': HOME}, 'data': INTEGRATIONS}},
        'favorites': FAVORITES,
        'row': row,
        'hgs_account': hgs_account,
        'heartbeat_account': heartbeat,
        'launch_account': launch,
        'stopped_account': stopped,
        'ascii_lengths': lengths,
        'stopped_hgs_account': stopped_hgs,
        'account_models': account_models(default),
    }


def main():
    text = json.dumps(build(), indent=2, ensure_ascii=True) + '\n'
    targets = [Path(sys.argv[1])] if len(sys.argv) > 1 else [
        Path('tests/fixtures/opencode-provider-catalog.json'),
        Path('mobile/android/app/src/test/resources/opencode-provider-catalog.json')]
    for target in targets:
        target.write_text(text, encoding='ascii')


if __name__ == '__main__':
    main()
