"""Capture pinned Windows libmpv digitally; no audio device or third-party Python modules.

Input: JSON with dll and captures [{source,output,sample_rate,start_seconds}].
The PCM AO is documented at https://mpv.io/manual/stable/#audio-output-drivers-pcm
"""
import ctypes as C
import hashlib
import json
from pathlib import Path
import sys
import time


class Event(C.Structure):
    _fields_=[('id',C.c_int),('error',C.c_int),('userdata',C.c_uint64),('data',C.c_void_p)]


class EndFile(C.Structure):
    _fields_=[('reason',C.c_int),('error',C.c_int)]


def capture(config):
    dll=Path(config['dll']).resolve(strict=True);lib=C.CDLL(str(dll))
    for name,result,args in [
        ('create',C.c_void_p,[]),('initialize',C.c_int,[C.c_void_p]),
        ('set_option_string',C.c_int,[C.c_void_p,C.c_char_p,C.c_char_p]),
        ('command',C.c_int,[C.c_void_p,C.POINTER(C.c_char_p)]),
        ('get_property',C.c_int,[C.c_void_p,C.c_char_p,C.c_int,C.c_void_p]),
        ('wait_event',C.POINTER(Event),[C.c_void_p,C.c_double]),
        ('terminate_destroy',None,[C.c_void_p]),('free',None,[C.c_void_p])]:
        function=getattr(lib,'mpv_'+name);function.restype=result;function.argtypes=args
    report={'dll_sha256':hashlib.sha256(dll.read_bytes()).hexdigest(),'captures':[]}
    for row in config['captures']:
        source=Path(row['source']).resolve(strict=True);output=Path(row['output']).resolve()
        if output==source or output.suffix!='.raw':raise ValueError('digital output must be a distinct raw fixture file')
        handle=lib.mpv_create();positions=[];started=time.monotonic()
        if not handle:raise RuntimeError('mpv context allocation failed')
        def check(code):
            if code<0:raise RuntimeError('mpv operation failed: '+str(code))
        try:
            options={'config':'no','video':'no','audio-display':'no','terminal':'no','load-scripts':'no','idle':'yes',
                'ao':'pcm','ao-pcm-waveheader':'no','ao-pcm-file':str(Path(row['output']).resolve()),
                'audio-format':'float','audio-samplerate':str(row['sample_rate']),'audio-channels':'stereo',
                'replaygain':'no','volume':'100'}
            options.update(row.get('options',{}))
            for key,value in options.items():check(lib.mpv_set_option_string(handle,key.encode(),value.encode('utf-8')))
            check(lib.mpv_initialize(handle))
            if 'builds' not in report:
                report['builds']={}
                for name in ('mpv-version','ffmpeg-version'):
                    text=C.c_void_p();check(lib.mpv_get_property(handle,name.encode(),1,C.byref(text)))
                    try:report['builds'][name]=C.cast(text,C.c_char_p).value.decode()
                    finally:lib.mpv_free(text)
            args=['loadfile',str(Path(row['source']).resolve()),'replace','-1','pause=no'+(',start='+str(row['start_seconds']) if row.get('start_seconds') is not None else '')]
            check(lib.mpv_command(handle,(C.c_char_p*(len(args)+1))(*[a.encode('utf-8') for a in args],None)))
            while time.monotonic()-started<15:
                event=lib.mpv_wait_event(handle,.01).contents
                position=C.c_double()
                if lib.mpv_get_property(handle,b'time-pos',5,C.byref(position))>=0:positions.append(position.value)
                if event.id==7:
                    end=C.cast(event.data,C.POINTER(EndFile)).contents
                    if event.error<0 or end.reason!=0 or end.error<0:raise RuntimeError('mpv did not reach a clean digital EOF')
                    break
            else:raise RuntimeError('digital capture timed out')
        finally:lib.mpv_terminate_destroy(handle)
        output=Path(row['output']);size=output.stat().st_size
        if not size or size%8:raise RuntimeError('invalid stereo float PCM capture')
        report['captures'].append({**row,'frames':size//8,'positions':positions,'seconds':time.monotonic()-started,
            'sha256':hashlib.sha256(output.read_bytes()).hexdigest()})
    return report


if __name__=='__main__':
    config=json.loads(Path(sys.argv[1]).read_text(encoding='utf-8-sig'))
    result=capture(config);Path(sys.argv[2]).write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps({'digital_captures':len(result['captures']),'dll_sha256':result['dll_sha256']}))
