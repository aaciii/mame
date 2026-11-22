#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
'Preserve the tested source and create one stock-0.289-to-current patch.'
import argparse, hashlib, json, os, re, shutil, subprocess, tempfile
from pathlib import Path
EXISTING = [
    'src/frontend/mame/ui/selmenu.h',
    'src/frontend/mame/ui/selmenu.cpp',
    'src/frontend/mame/ui/menu.h',
    'src/frontend/mame/ui/menu.cpp',
    'src/frontend/mame/ui/moptions.h',
    'src/frontend/mame/ui/moptions.cpp',
    'scripts/src/mame/frontend.lua',
    'scripts/src/main.lua',
    'src/frontend/mame/ui/dirmenu.cpp',
]
NEW = ['src/frontend/mame/ui/'+name for name in (
 'videosnap_player.h','videosnap_player.cpp','videosnap_paths.h',
 'videosnap_paths.cpp','videosnap_menu.ipp','videosnap_audio.h','videosnap_audio.cpp')]
FILES = EXISTING + NEW

def sha256(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()
def run(args,cwd=None,env=None):
    p=subprocess.run(args,cwd=cwd,env=env,capture_output=True)
    if p.returncode:raise RuntimeError('Command failed: '+' '.join(map(str,args))+'\n'+p.stderr.decode('utf-8','replace'))
    return p.stdout
def capture(args,cwd):
    try:
        p=subprocess.run(args,cwd=cwd,capture_output=True,text=True,errors='replace')
        return '$ '+' '.join(args)+'\n'+p.stdout+p.stderr+'\nExit: '+str(p.returncode)+'\n'
    except OSError as e:return '$ '+' '.join(args)+'\nUnavailable: '+str(e)+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--source-archive',action='store_true',help='Archive full modified MAME source; needs additional disk space')
    a=p.parse_args();root=a.source.resolve();dest=a.output.resolve()
    if dest.exists():raise RuntimeError('Output already exists; choose a fresh output directory')
    if dest.is_relative_to(root):raise RuntimeError('Output must be outside the source checkout')
    def git(*args,env=None):return run(['git','-C',str(root),*args],env=env)
    base=git('rev-list','-n','1','refs/tags/mame0289').decode().strip()
    for name in FILES:
        if not (root/name).is_file():raise RuntimeError('Missing required feature source: '+name)
    header=(root/'src/frontend/mame/ui/moptions.h').read_text(encoding='utf-8')
    for macro,key in [('OPTION_VIDEO_DIRECTORY','video_directory'),('OPTION_VIDEO_ENABLE_AUDIO','video_enable_audio'),('OPTION_VIDEO_VOLUME','video_volume')]:
        if len(re.findall(r'^\s*#define\s+'+macro+r'\s+"'+key+r'"\s*$',header,re.M))!=1:
            raise RuntimeError('Canonical definition missing or duplicated: '+macro)
    for name in FILES:
        text=(root/name).read_text(encoding='utf-8')
        for old in ('OPTION_VIDEOSNAP_PATH','OPTION_VIDEOSNAP_AUDIO','OPTION_VIDEOSNAP_VOLUME','videosnappath','videosnapaudio','videosnapvolume'):
            if re.search(r'\b'+old+r'\b',text):raise RuntimeError('Legacy option in '+name+': '+old)
        for old in ('videosnap_path','videosnap_audio','videosnap_volume'):
            if re.search(r'\b'+old+r'\s*\(',text):raise RuntimeError('Legacy accessor in '+name+': '+old)
    tracked=git('diff','--name-only',base,'--').decode('utf-8').splitlines()
    unexpected=set(tracked)-set(FILES)
    if unexpected:raise RuntimeError('Unexpected tracked changes; review first: '+', '.join(sorted(unexpected)))
    unknown=git('ls-files','--others','--exclude-standard','--','src','scripts').decode('utf-8').splitlines()
    unknown_source=[n for n in unknown if Path(n).suffix in ('.h','.cpp','.ipp','.lua') and n not in FILES]
    if unknown_source:raise RuntimeError('Unrecognized untracked source: '+', '.join(unknown_source))
    submodules=[line for line in git('ls-tree','-r',base).decode('utf-8').splitlines() if line.startswith('160000 ')]
    if submodules:raise RuntimeError('Submodules need a separate archive plan: '+str(submodules))
    fingerprints={name:sha256(root/name) for name in FILES}
    dest.mkdir(parents=True)
    with tempfile.TemporaryDirectory() as temp:
        env=dict(os.environ);env['GIT_INDEX_FILE']=str(Path(temp)/'release.index')
        git('read-tree',base,env=env)
        git('add','--',*FILES,env=env)
        tree=git('write-tree',env=env).decode().strip()
        patch=dest/'mame0289-video-preview.patch'
        patch.write_bytes(git('diff','--cached','--binary','--full-index',base,'--',env=env))
        if not patch.stat().st_size:raise RuntimeError('Patch is unexpectedly empty')
        git('read-tree',base,env=env)
        git('apply','--cached','--check',str(patch),env=env)
        git('apply','--cached',str(patch),env=env)
        if git('write-tree',env=env).decode().strip()!=tree:
            raise RuntimeError('Patch round-trip did not reproduce the snapshot tree')
        if a.source_archive:
            print('Creating the full modified source archive; this can take a while...',flush=True)
            git('archive','--format=tar.gz','--prefix=mame0289-video-preview/',
                '--output='+str(dest/'mame0289-video-preview-source.tar.gz'),tree)
    for name,digest in fingerprints.items():
        if sha256(root/name)!=digest:raise RuntimeError('Source changed during snapshot: '+name)
    binaries=[]
    for name in ('videotest.exe','mame.exe'):
        file=root/name
        if file.exists():
            saved=dest/('baseline-'+name);shutil.copy2(file,saved)
            binaries.append({'file':saved.name,'sha256':sha256(saved),'note':'Existing pre-full-build executable, not necessarily the release build'})
    report='\n'.join(capture(cmd,root) for cmd in [
        ['git','--version'],['gcc','--version'],['make','--version'],
        ['pkg-config','--modversion','libavformat','libavcodec','libavutil','libswscale','libswresample'],
        ['ffmpeg','-version'],['ffmpeg','-buildconf'],['pacman','-Q'],
    ])
    (dest/'build-environment.txt').write_text(report,encoding='utf-8')
    manifest={'base_tag':'mame0289','base_commit':base,'snapshot_tree':tree,
        'patch':patch.name,'patch_sha256':sha256(patch),'patch_roundtrip_verified':True,
        'feature_file_sha256':fingerprints,'existing_binaries_preserved':binaries,
        'full_source_archive':a.source_archive,
        'notes':['Real Git index, branch and HEAD unchanged; Git objects may be created.',
                 'Consolidated patch excludes user configuration, media, builds and old helper installers.',
                 'MAME source archive excludes external FFmpeg/runtime-library corresponding source.',
                 'Review environment report for local package details before publishing.']}
    (dest/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    instructions='''CONSOLIDATED SOURCE APPLICATION
Start with a clean standard mame0289 checkout.
  git apply --check /path/to/mame0289-video-preview.patch
  git apply /path/to/mame0289-video-preview.patch
Do not apply the old incremental installers afterward.

Windows UCRT64 full build (FFmpeg development libraries and pkg-config required):
  MAME_VIDEO_SNAPS=1 make -j2 TARGET=mame SUBTARGET=mame BUILDDIR=build-release REGENIE=1
Do not pass SOURCES or SOURCEFILTER; inspect useroptions.mak for filters.
PortAudio must not be disabled.

This is a development release candidate, not official MAME.
Binary redistribution still needs external-library source and license review,
recursive DLL collection, clean-machine validation, and final packaging.
'''
    (dest/'BUILDING.txt').write_text(instructions,encoding='utf-8')
    print('Snapshot complete. Patch round-trip verified against stock '+base)
    print('Snapshot tree: '+tree)
    print('Release work directory: '+str(dest))
if __name__=='__main__':
    try:main()
    except (OSError,RuntimeError) as e:raise SystemExit('ERROR: '+str(e))
