"""Limited source tripwire, not a proof of RT safety or an OS lock interceptor."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
checks = [('src/ContinuousEngine.cpp', 'ContinuousEngine::processBlock'),
          ('src/ContinuousEngine.cpp', 'ContinuousEngine::requestMode'),
          ('src/Biquad.cpp', 'Biquad::tryProcessSample')]
forbidden = re.compile(r'\b(new|delete|throw|malloc|calloc|realloc|free|mutex|lock|sleep|wait|printf|cout|cerr|fopen|fstream|ffmpeg|socket|prepare|reset|sin|cos|pow|log|exp)\b|EnterCriticalSection|WaitFor|Sleep\s*\(', re.I)
for filename, name in checks:
    source = (root / filename).read_text(encoding='utf-8')
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    start = source.index('{', source.index(name + '('))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    body = source[start:end]
    match = forbidden.search(body)
    if match:
        raise AssertionError(f'{name}: forbidden RT-path token {match.group()}')
    calls = re.findall(r'([A-Za-z_]\w*(?:::\w+)*)\s*\(', body)
    allowed = {'if', 'for', 'while', 'static_cast', 'reinterpret_cast', 'alignof', 'sizeof',
               'size', 'max', '::max', 'std::isfinite', 'std::abs', 'std::clamp', 'tryProcessSample'}
    unexpected = set(calls) - allowed
    if unexpected:
        raise AssertionError(f'{name}: new calls require manual RT audit: {unexpected}')
    print(f'PASS limited source audit: {name}; {len(calls)} calls inspected')
# Check the detector itself (no execution of these forbidden APIs).
for token in ('std::mutex', 'malloc(16)', 'Sleep(1)', 'filter.prepare(48000)', 'throw x', 'std::cos(x)'):
    assert forbidden.search(token), token
print('Scope: source functions + explicit call whitelist; no universal lock/allocation interception')
