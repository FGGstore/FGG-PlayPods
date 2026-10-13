#!/usr/bin/env python3
"""Host-only startup guidance, bounded ownership recovery and audio regressions."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run():
    out = ROOT / 'build/resident-offline'
    out.mkdir(parents=True, exist_ok=True)
    candidate = ROOT / 'src'
    flags = ['gcc', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror',
             '-g', '-Isrc', '-Ithird_party/sbc', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-DHCI_OUT_WAIT_REARM=0', '-ffunction-sections', '-fdata-sections']
    cases = ['immediate', 'late', 'delayed-sdp', 'near-deadline', 'wrong-label',
             'wrong-signal', 'unanswered', 'other-command', 'loss', 'send-failed']
    fixtures = []
    for name, folder in [('candidate', candidate)]:
        definition = '-DA2DP_TEST_SOURCE="' + str(folder / 'a2dp.c') + '"'
        fixtures.append((name, ['tests/avdtp_reply_window_test.c', 'src/sdp.c', definition]
                         + (['-DBASELINE_WINDOW'] if name == 'baseline' else []), cases))
    # Candidate AVDTP replies must retain all earlier peer/collision controls.
    common = (ROOT / 'tests/play_connection_test.c').read_text()
    for name in ['bt.c', 'a2dp.c']:
        common = common.replace('../src/' + name, str(candidate / name))
    common += '\nstatic int output_idle=1;\nint hci_output_idle(void){return output_idle;}\n'
    path = out / 'connection-fixture.c'
    path.write_text(common)
    fixtures.append(('connection', [str(path), 'src/sdp.c'],
                     ['incoming', 'collision-refused', 'collision-accepted',
                      'collision-unanswered', 'collision-close-timeout', 'collision-invalid-cid',
                      'collision-peer-close', 'invalid-busy', 'reply-send-failure',
                      'connection-state', 'early-authentication', 'cleanup-success',
                      'cleanup-failed', 'cleanup-timeout', 'matched-channel-close', 'close-once']))
    startup = common.replace('void notify(const char *fmt,...) {(void)fmt;}',
                             'void notify(const char *fmt,...);')
    startup = startup.replace('int hci_pump(int ms)', 'static int common_hci_pump(int ms)')
    if startup == common or 'static int common_hci_pump' not in startup:
        raise ValueError('unexpected common harness')
    (out / 'startup-harness.c').write_text(startup)
    startup_test = (ROOT / 'tests/startup_guidance_test.c').read_text().replace(
        '../build/resident-offline/startup-harness.c', str(out / 'startup-harness.c'))
    (out / 'startup-test.c').write_text(startup_test)
    fixtures.append(('startup', [str(out / 'startup-test.c'), 'src/sdp.c'],
                     ['immediate', 'spontaneous', 'restart', 'timeout', 'foreign',
                      'transport-stop', 'near-deadline', 'auth-failed']))
    resident = common.replace('int hci_pump(int ms)', 'static int common_hci_pump(int ms)')
    resident = resident.replace('if(g_sent>g_reported)',
                                'if(!g_disconnected && g_sent>g_reported+g_disconnect_retired)')
    resident = resident.replace('(g_sent-g_reported)', '(g_sent-g_reported-g_disconnect_retired)')
    (out / 'resident-harness.c').write_text(resident)
    fixtures.append(('resident', ['tests/resident_reconnect_test.c', 'src/sdp.c'],
        ['resume','timeout','late-excess','wait-stop','unobserved','failed-disconnect',
         'foreign-disconnect','out-busy','setup-fault','transport-fault','credit-fault',
         'foreign-command-fault','stopping','missing-key','queued-success','late-completion']))
    fixtures.append(('main', ['tests/resident_main_test.c'],
        ['resume','no-rearm','audio-rearm-failed','no-peer','capture-not-ready',
         'second-auth-failed','second-audio-failed']))
    hci = (ROOT / 'tests/hci_send_test.c').read_text().replace('../src/hci.c', str(candidate / 'hci.c'))
    (out / 'hci-harness.c').write_text(hci)
    fixtures.append(('hci-idle', ['tests/resident_output_idle_test.c', '-Itests/stubs'], ['']))
    fixtures.append(('hci-send', [str(out / 'hci-harness.c'), '-Itests/stubs'], ['']))
    fixtures.append(('hci-send-a', [str(out / 'hci-harness.c'), '-Itests/stubs', '-UHCI_OUT_WAIT_REARM', '-DHCI_OUT_WAIT_REARM=1'], ['']))
    bt = '-DBT_TEST_SOURCE="' + str(candidate / 'bt.c') + '"'
    fixtures.append(('completion-policy', ['tests/completion_policy_test.c', 'src/sdp.c', bt], ['']))
    fixtures.append(('active-mode', ['tests/active_mode_test.c', 'src/sdp.c', bt], ['']))
    fixtures.append(('continuous', ['tests/continuous_stream_test.c',
                     '-DA2DP_TEST_SOURCE="' + str(candidate / 'a2dp.c') + '"'], ['']))
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
    transcript, results = [], []
    for name, sources, invocations in fixtures:
        command = flags + sources + ['-Wl,--gc-sections', '-o', str(out / name)]
        built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=30)
        transcript.append(json.dumps(command) + '\n' + built.stdout + built.stderr)
        (out / 'results.log').write_text('\n'.join(transcript))
        built.check_returncode()
        for case in invocations:
            command = [str(out / name)] + ([case] if case else [])
            checked = subprocess.run(command, cwd=ROOT, env=env, capture_output=True,
                                     text=True, timeout=30)
            transcript.append(json.dumps(command) + '\n' + checked.stdout + checked.stderr)
            (out / 'results.log').write_text('\n'.join(transcript))
            checked.check_returncode()
            results.append({'fixture': name, 'case': case or 'default', 'passed': True,
                            'output': checked.stdout.strip()})
    sources = sorted((ROOT / 'tests').rglob('*.[ch]')) + sorted(candidate.glob('*.[ch]'))
    sources += [Path(__file__), ROOT / 'Makefile']
    record = {'host_invocations': len(results), 'sanitizers': ['ASan', 'UBSan'],
              'target_execution': False, 'results': results,
              'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in sources}}
    (out / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
    assert len(results) == 63
    print('Passed', len(results), 'host invocations; no target executed')


if __name__ == '__main__':
    run()
