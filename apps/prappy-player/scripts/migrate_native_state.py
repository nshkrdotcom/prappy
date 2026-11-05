"""One-time native state conversion; normal player code has no retired reader.

The backend supplies verified old timelines, native manifests and exact region
revision mappings. Conversion is offline, after outstanding saves have settled.
"""
from copy import deepcopy
import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PureWindowsPath
import re
import tempfile


def source_key(source):
    for name,prefix in (('library_id','lib'),('capture_id','cap'),('timeline_id','tl')):
        check_id(source[name],prefix)
    return '/'.join(source[name] for name in ('library_id','capture_id','timeline_id'))


def check_id(value,prefix):
    if not isinstance(value,str) or not re.fullmatch(prefix+r'_[0-9a-f]{32}',value):
        raise ValueError('invalid canonical '+prefix+' identity')


class Clock:
    def __init__(self,old,native):
        self.old_rate,self.rate=old['sample_rate'],native['sample_rate']
        self.old_total,self.total=old['frame_count'],native['frame_count']
        if any(type(value) is not int or value<=0 or value>2**63-1 for value in (
            self.old_rate,self.rate,self.old_total,self.total)):
            raise ValueError('invalid source clock')
        if max(self.old_rate,self.rate)>768000 or abs(self.round(self.old_total)-self.total)>(self.rate+self.old_rate-1)//self.old_rate+1:
            raise ValueError('source duration changed during native migration')

    def round(self,value):return (2*value*self.rate+self.old_rate)//(2*self.old_rate)

    def frame(self,value):
        if type(value) is not int or not 0<=value<=self.old_total:raise ValueError('source frame is out of bounds')
        result=self.total if value==self.old_total else self.round(value)
        if not 0<=result<=self.total:raise ValueError('native frame is out of bounds')
        return result

    def viewport(self,value):
        if type(value) not in (int,float) or not math.isfinite(value):raise ValueError('invalid viewport')
        return self.total if value==self.old_total else value*self.rate/self.old_rate


def migrate_workspace(original,bundle):
    if bundle['schema_version']!=1:raise ValueError('unsupported migration manifest')
    if original['commands'] or original.get('source_handoff') is not None:
        raise ValueError('pending commands and source handoffs must finish before migration')
    mappings={}
    for item in bundle['sources']:
        old=item['timeline']; prepared=item['prepared']; capture=prepared['capture']; native=capture['timeline']
        old_ref={'library_id':item['library_id'],'capture_id':old['capture_id'],'timeline_id':old['id']}
        if (prepared['state']!='Ready' or capture['id']!=old['capture_id'] or
            capture['timeline_id']!=native['id'] or native['working_asset_key']!=prepared['key'] or
            capture['blob_sha256']!=prepared['source_sha256']):
            raise ValueError('native manifest does not describe the saved original')
        key=source_key(old_ref); new_ref={**old_ref,'timeline_id':native['id']}; source_key(new_ref)
        if key in mappings:raise ValueError('duplicate source mapping')
        mappings[key]=(item,Clock(old,native),new_ref)
    regions=bundle['region_revisions']
    def region_ref(library,reference):
        key=library+'/'+reference['id']+':'+str(reference['revision'])
        if key not in regions:raise ValueError('saved region has no native revision mapping')
        return {**reference,**regions[key]}
    def draft_value(library,value,clock):
        converted=deepcopy(value)
        converted['start']=clock.frame(value['start']); converted['end']=clock.frame(value['end'])
        if converted['start']>=converted['end']:raise ValueError('saved draft would collapse in the native clock')
        attributes=converted['attributes']; target=attributes.get('metadata_target','')
        if target.startswith('reg_'):
            attributes['metadata_target_revision']=region_ref(library,{
                'id':target,'revision':attributes['metadata_target_revision']})['revision']
        metadata=attributes.get('metadata',{})
        if metadata.get('target_id','').startswith('reg_'):
            metadata['target_revision']=region_ref(library,{
                'id':metadata['target_id'],'revision':metadata['target_revision']})['revision']
        return converted
    result=deepcopy(original)
    for name in ('source_handoff_digest','source_handoff_library','source_handoff_request'):result.pop(name,None)
    result['source_handoff_source']=None; result['prepared_manifest']=None
    for row in result['workspaces']['sources']:
        key=source_key(row['source'])
        if key not in mappings:raise ValueError('saved editor source is unmapped')
        item,clock,new_ref=mappings[key]; library=new_ref['library_id']
        if row['total']!=clock.old_total:raise ValueError('workspace differs from its registered timeline')
        row['total']=clock.total; row['source']=new_ref
        view=row['view']; view['capture']=deepcopy(item['prepared']['capture'])
        if 'cursor' in view:view['cursor']=min(clock.total-1,clock.frame(view['cursor']))
        for name in ('viewport_start','viewport_end'):
            if name in view:view[name]=clock.viewport(view[name])
        for draft in row['drafts']:
            if draft['saving'] is not None:raise ValueError('pending draft saves must finish before migration')
            was_dirty=draft['baseline'] is None or draft['value']!=draft['baseline']
            draft['value']=draft_value(library,draft['value'],clock)
            if draft['baseline'] is not None:draft['baseline']=draft_value(library,draft['baseline'],clock)
            if draft['region'] is not None:draft['region']=region_ref(library,draft['region'])
            if was_dirty!=(draft['baseline'] is None or draft['value']!=draft['baseline']):
                raise ValueError('native rounding would hide an unsaved edit; preserve it explicitly before migration')
    if result['active_source'] is not None:
        item,_,new_ref=mappings[source_key(result['active_source'])]
        result['active_source']=new_ref; result['prepared_manifest']=deepcopy(item['prepared'])
    return result


def migrate_history(text,path_map,windows_state_directory):
    lines=text.splitlines(keepends=True)
    if not lines or lines[0].rstrip('\r\n')!='PRAPPY_HISTORY 1':raise ValueError('unsupported history version')
    for index,line in enumerate(lines):
        if not line.startswith(('ENTRY ','QUEUED ')):continue
        match=re.fullmatch(r'((?:ENTRY|QUEUED) \d+ -?\d+ \S+ )(".*")(\r?\n)?',line)
        if match is None:raise ValueError('invalid history entry')
        path=json.loads(match[2])
        if path not in path_map:raise ValueError('unmapped history audio: '+path)
        source=path_map[path]
        check_id(source['library_id'],'lib'); check_id(source['capture_id'],'cap')
        target=PureWindowsPath(windows_state_directory)/'sources'/source['library_id']/(source['capture_id']+'.source')
        lines[index]=match[1]+json.dumps(str(target),ensure_ascii=False)+(match[3] or '')
    return ''.join(lines)


def migrate_preferences(text,codecs):
    lines=text.splitlines()
    if not lines or lines[0]!='PRAPPY_UI 4':raise ValueError('unsupported preferences version')
    cursor=3
    def skip_paths():
        nonlocal cursor
        count=int(lines[cursor]); cursor+=1
        if not 0<=count<=4096 or cursor+count>len(lines):raise ValueError('invalid preference path list')
        cursor+=count
    skip_paths(); skip_paths()
    volume=lines[cursor]; cursor+=1
    if not 0<=int(volume)<=100:raise ValueError('invalid saved volume')
    session=lines[cursor:cursor+3]; cursor+=3
    strings=[]; remainder=lines[cursor]; cursor+=1
    decoder=json.JSONDecoder()
    for _ in range(4):
        value,end=decoder.raw_decode(remainder.lstrip()); strings.append(value)
        remainder=remainder.lstrip()[end:]
    if remainder.strip() or any(not isinstance(value,str) for value in strings):raise ValueError('invalid preference filters')
    if strings[1].removeprefix('.') not in codecs:strings[1]=''
    strings[2:]=['','']
    scroll=lines[cursor]; cursor+=1
    skip_paths()
    groups=int(lines[cursor]); cursor+=1
    if not 0<=groups<=500000 or cursor+groups>len(lines):raise ValueError('invalid saved groups')
    cursor+=groups
    if any(line.strip() for line in lines[cursor:]):raise ValueError('unexpected preferences data')
    # Filesystem folder scopes cannot identify the new catalog's library groups.
    # Their original bytes remain archived; user volume/layout/search are retained.
    return '\n'.join([lines[0],lines[1],'""','0','0',volume,*session,
        ' '.join(json.dumps(value,ensure_ascii=False) for value in strings),scroll,'0','0'])+'\n'


def plan_state(directory,bundle):
    directory=Path(directory).resolve(); files=[]
    for name in ('sample-workspaces.json','player-history.txt','player-ui.txt'):
        path=directory/name
        if not path.is_file():continue
        original=path.read_bytes(); text=original.decode('utf-8-sig')
        if name=='sample-workspaces.json':
            text=json.dumps(migrate_workspace(json.loads(text),bundle),indent=2,ensure_ascii=False)+'\n'
        elif name=='player-history.txt':
            text=migrate_history(text,bundle['history_paths'],bundle['windows_state_directory'])
        else:text=migrate_preferences(text,set(bundle['codecs']))
        files.append({'name':name,'original_sha256':hashlib.sha256(original).hexdigest(),'new_text':text})
    return {'schema_version':1,'state_directory':str(directory),'files':files}


def atomic_write(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    descriptor,name=tempfile.mkstemp(prefix='.native-state-',dir=path.parent)
    try:
        with os.fdopen(descriptor,'wb') as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.replace(name,path)
        directory=os.open(path.parent,os.O_RDONLY)
        try:os.fsync(directory)
        finally:os.close(directory)
    finally:Path(name).unlink(missing_ok=True)


def apply_plan(plan,*,fail_after=None):
    if plan['schema_version']!=1:raise ValueError('unsupported state migration plan')
    root=Path(plan['state_directory']).resolve()
    identity=hashlib.sha256(json.dumps(plan,sort_keys=True,ensure_ascii=False).encode()).hexdigest()
    archive=(root/'native-state-originals'/identity).resolve()
    if not archive.is_relative_to(root):raise ValueError('state archive leaves its directory')
    changes=[]
    for item in plan['files']:
        name=item['name']
        if name not in {'sample-workspaces.json','player-history.txt','player-ui.txt'}:
            raise ValueError('unexpected state migration file')
        path=root/name; backup=archive/(name+'.before'); data=item['new_text'].encode('utf-8')
        if path.is_symlink():raise ValueError('state migration cannot replace a symbolic link')
        original=path.read_bytes()
        if backup.exists():
            if hashlib.sha256(backup.read_bytes()).hexdigest()!=item['original_sha256']:
                raise ValueError('original state archive changed')
            if original!=data and hashlib.sha256(original).hexdigest()!=item['original_sha256']:
                raise ValueError('saved state changed after planning')
        elif hashlib.sha256(original).hexdigest()==item['original_sha256']:
            changes.append((backup,original))
        else:raise ValueError('saved state changed before archival')
    for path,data in changes:atomic_write(path,data)
    atomic_write(archive/'plan.json',json.dumps(plan,indent=2,ensure_ascii=False).encode('utf-8'))
    for index,item in enumerate(plan['files'],1):
        atomic_write(root/item['name'],item['new_text'].encode('utf-8'))
        if fail_after==index:raise RuntimeError('injected state migration interruption')
    return {'applied':True,'archive':str(archive),'files':len(plan['files'])}


def main():
    parser=argparse.ArgumentParser(description='Plan/apply offline native player-state conversion; keep the player closed')
    parser.add_argument('--state-dir',type=Path)
    parser.add_argument('--mapping',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--apply-plan',type=Path)
    args=parser.parse_args()
    if args.apply_plan:
        if args.state_dir or args.mapping or args.output:parser.error('apply-plan cannot be combined with planning arguments')
        print(json.dumps(apply_plan(json.loads(args.apply_plan.read_bytes())))); return
    if not all((args.state_dir,args.mapping,args.output)):parser.error('planning requires state-dir, mapping and a new output path')
    plan=plan_state(args.state_dir,json.loads(args.mapping.read_bytes()))
    with args.output.open('x',encoding='utf-8') as stream:json.dump(plan,stream,indent=2,ensure_ascii=False)
    print(json.dumps({'plan':str(args.output),'files':len(plan['files']),'applied':False}))


if __name__=='__main__':main()
