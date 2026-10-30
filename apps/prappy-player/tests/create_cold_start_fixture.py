"""Create an isolated real sampling server with a test-only HTTP response gate.

Run under scraper's pinned WSL environment, with PYTHONPATH pointing to its src:
  python create_cold_start_fixture.py --port 18782 --manifest /mnt/c/.../fixture.json

The returned manifest records this fixture's PID. Never stop the production server
or WSL to simulate a cold service. Authenticated POST gate_url + /hold, /release,
or /cancel control only this fixture; GET /state observes the barrier. Cancel
returns HTTP 503 to requests already held, while release lets them complete.
"""

import argparse
import asyncio
import hmac
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from urllib.error import URLError
from urllib.request import Request, urlopen


def windows_path(path):
    path = Path(path).resolve()
    parts = path.parts
    if len(parts) >= 3 and parts[1] == 'mnt' and len(parts[2]) == 1:
        return parts[2].upper() + ':\\' + '\\'.join(parts[3:])
    return '\\\\wsl.localhost\\ubuntu26-dev' + str(path).replace('/', '\\')


def request_json(url, token, *, method='GET', timeout=2):
    request = Request(url, method=method, headers={'Authorization': 'Bearer ' + token})
    with urlopen(request, timeout=timeout) as response:
        return json.load(response)


def gated_app(config):
    from fastapi import Depends, Header, HTTPException
    from fastapi.responses import JSONResponse
    from audioharvest.server import create_app

    token = Path(config['token_file']).read_text().strip()
    app = create_app([Path(config['library'])], token,
                     source_roots=[Path(config['directory']) / 'media'])
    gate = asyncio.Event()
    gate.set()
    held_requests = waiting_requests = cancelled_requests = cancellation_epoch = 0

    def state():
        return {'ready': True, 'pid': os.getpid(), 'library_id': config['library_id'],
                'holding': not gate.is_set(), 'held_requests': held_requests,
                'waiting_requests': waiting_requests, 'cancelled_requests': cancelled_requests}

    def authenticate(authorization: str = Header(default='')):
        if not hmac.compare_digest(authorization, 'Bearer ' + token):
            raise HTTPException(401, 'invalid fixture credentials')

    @app.middleware('http')
    async def hold_product_requests(request, call_next):
        nonlocal held_requests, waiting_requests, cancelled_requests
        if request.url.path.startswith('/v1/') and not gate.is_set():
            if not hmac.compare_digest(request.headers.get('Authorization', ''), 'Bearer ' + token):
                return JSONResponse({'error': 'invalid credentials'}, status_code=401)
            epoch = cancellation_epoch
            held_requests += 1
            waiting_requests += 1
            try:
                await gate.wait()
            finally:
                waiting_requests -= 1
            if epoch != cancellation_epoch:
                cancelled_requests += 1
                return JSONResponse({'error': {'code': 'fixture_cancelled',
                                              'message': 'Held fixture request cancelled'}}, status_code=503)
        return await call_next(request)

    @app.get('/_fixture/state', dependencies=[Depends(authenticate)])
    async def inspect_gate():
        return state()

    @app.post('/_fixture/hold', dependencies=[Depends(authenticate)])
    async def hold_gate():
        gate.clear()
        return state()

    @app.post('/_fixture/release', dependencies=[Depends(authenticate)])
    async def release_gate():
        gate.set()
        return state()

    @app.post('/_fixture/cancel', dependencies=[Depends(authenticate)])
    async def cancel_gate():
        nonlocal cancellation_epoch
        cancellation_epoch += 1
        gate.set()
        return state()

    return app


def serve(state_path):
    import uvicorn
    config = json.loads(state_path.read_text())
    uvicorn.run(gated_app(config), host='127.0.0.1', port=config['port'],
                log_level='warning', access_log=False)


def create(port, manifest, directory):
    import numpy as np
    import soundfile as sf
    from audioharvest.catalog.commits import Catalog
    from audioharvest.catalog.config import init_library
    from audioharvest.services.sources import SourceService

    if port == 8765 or not 1024 <= port <= 65535:
        raise ValueError('choose a disposable port from 1024 through 65535, excluding production port 8765')
    if manifest.exists():
        raise FileExistsError(f'manifest already exists: {manifest}')
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', port))
    if directory is None:
        directory = Path(tempfile.mkdtemp(prefix='prappy-cold-start-'))
    else:
        directory.mkdir(parents=True, exist_ok=False)
    directory = directory.resolve()
    media = directory / 'media'
    media.mkdir()
    rate = 48000
    frames = np.arange(rate * 15, dtype=np.float64)
    for name, hz in [('previous', 440), ('mark', 660)]:
        # The standard editor regression navigates from the loud opening to a
        # quieter final passage to verify viewport-dependent amplitude fitting.
        amplitude = np.where(frames < 9 * rate, .2, .02)
        tone = (amplitude * np.sin(frames * (2 * np.pi * hz / rate))).astype('float32')
        sf.write(media / (name + '.wav'), np.column_stack((tone, tone)), rate, subtype='PCM_16')
    library = directory / 'library'
    init_library(library, registry=directory / 'registry.json')
    service = SourceService(library)
    previous = service.import_file(media / 'previous.wav', complete=True, title='Previous 440 Hz')
    marked = service.import_file(media / 'mark.wav', complete=True, title='Marked 660 Hz')
    library_id = Catalog(library).config.id
    token = secrets.token_urlsafe(48)
    token_file = directory / 'server.token'
    with os.fdopen(os.open(token_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as stream:
        stream.write(token + '\n')
    config = {'directory': str(directory), 'library': str(library), 'library_id': library_id,
              'token_file': str(token_file), 'port': port}
    state_path = directory / 'fixture-server.json'
    state_path.write_text(json.dumps(config, indent=2) + '\n')
    url = f'http://127.0.0.1:{port}'
    log = directory / 'server.log'
    with log.open('ab') as output:
        child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                  '--serve-state', str(state_path)],
                                 stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                                 start_new_session=True)
    (directory / 'server.pid').write_text(str(child.pid) + '\n')
    try:
        deadline = time.monotonic() + 30
        retry = threading.Event()
        while True:
            if child.poll() is not None:
                raise RuntimeError(f'fixture server exited; inspect {log}')
            try:
                ready = request_json(url + '/_fixture/state', token, timeout=.5)
                if ready['ready'] and ready['pid'] == child.pid:
                    break
            except (URLError, TimeoutError, ConnectionError):
                pass
            if time.monotonic() >= deadline:
                raise TimeoutError(f'fixture server did not become ready; inspect {log}')
            retry.wait(.05)
        previous_capture = request_json(url + '/v1/libraries/' + library_id + '/entities/' + previous['capture_id'], token)
        result = {'url': url, 'token_file': windows_path(token_file), 'library_id': library_id,
                  'previous_capture': previous_capture, 'mark_song': windows_path(media / 'mark.wav'),
                  'mark_capture_id': marked['capture_id'], 'tone_hz': 660, 'gate_url': url + '/_fixture',
                  'fixture_pid': child.pid, 'fixture_directory': windows_path(directory),
                  'fixture_log': windows_path(log)}
        manifest.parent.mkdir(parents=True, exist_ok=True)
        with manifest.open('x') as stream:
            stream.write(json.dumps(result, indent=2) + '\n')
        print(json.dumps({'manifest': windows_path(manifest), 'url': url,
                          'pid': child.pid, 'gate_url': result['gate_url']}))
    except BaseException:
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        child.wait(timeout=10)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--directory', type=Path, help='new disposable Linux directory; defaults to /tmp')
    parser.add_argument('--serve-state', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.serve_state:
        serve(args.serve_state)
    else:
        if args.port is None or args.manifest is None:
            parser.error('--port and --manifest are required')
        create(args.port, args.manifest.resolve(), args.directory)


if __name__ == '__main__':
    main()
