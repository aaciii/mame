#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Audit recursive PE imports and optionally stage an unpublished Windows overlay."""
import argparse,hashlib,json,os,re,shutil,subprocess
from pathlib import Path

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exe',required=True,type=Path)
    p.add_argument('--runtime-bin',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--stage',action='store_true',help='Copy only after all static imports resolve; not a publishable release')
    a=p.parse_args();exe=a.exe.resolve();runtime=a.runtime_bin.resolve();dest=a.output.resolve()
    if not exe.is_file() or exe.name.lower()!='mame.exe':raise RuntimeError('Supply the full-build mame.exe, not videotest.exe')
    if not runtime.is_dir():raise RuntimeError('Runtime directory missing: '+str(runtime))
    if dest.exists():raise RuntimeError('Output directory already exists; choose a fresh one')
    objdump=shutil.which('objdump')
    if not objdump:raise RuntimeError('objdump not found; run from UCRT64')
    windir=os.environ.get('WINDIR') or os.environ.get('SystemRoot')
    if not windir:raise RuntimeError('Run on Windows so system DLLs can be identified')
    system=Path(windir)/'System32'
    def directory_map(folder):return {f.name.lower():f for f in folder.iterdir() if f.is_file()} if folder.is_dir() else {}
    local=directory_map(exe.parent);binaries=directory_map(runtime);osfiles=directory_map(system)
    queue=[exe];seen=set();bundled={};edges=[];missing=[]
    while queue:
        file=queue.pop(0)
        identity=str(file).lower()
        if identity in seen:continue
        seen.add(identity)
        result=subprocess.run([objdump,'-p',str(file)],capture_output=True,text=True,errors='replace')
        if result.returncode:raise RuntimeError('Cannot inspect '+str(file)+'\n'+result.stderr)
        if file==exe and 'pei-x86-64' not in result.stdout:
            raise RuntimeError('Expected Windows x64 PE executable; inspect architecture before packaging')
        for name in re.findall(r'DLL Name:\s*([^\r\n]+)',result.stdout,re.I):
            name=name.strip();key=name.lower()
            record={'importer':file.name,'dll':name}
            if key.startswith(('msys-','cygwin')):raise RuntimeError('Unexpected MSYS/Cygwin runtime import: '+name)
            if key.startswith(('api-ms-win-','ext-ms-win-')):
                record['resolution']='Windows API set'
            elif key in local or key in binaries:
                found=local.get(key) or binaries[key]
                record['resolution']=str(found)
                if key not in bundled:bundled[key]=found;queue.append(found)
            elif key in osfiles:
                record['resolution']='Present in development System32; clean-machine availability still requires testing'
            else:
                record['resolution']='UNRESOLVED';missing.append(record.copy())
            edges.append(record)
    report={'executable':str(exe),'executable_sha256':digest(exe),
        'runtime_bin':str(runtime),'bundled_dlls':[{'file':f.name,'source':str(f),'sha256':digest(f)} for f in bundled.values()],
        'imports':edges,'unresolved':missing,'scope':'Static PE import audit only. Dynamic loads, clean-system availability, licenses and corresponding source remain to verify.'}
    dest.parent.mkdir(parents=True,exist_ok=True)
    reportpath=Path(str(dest)+'.dependencies.json')
    reportpath.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('Recursive DLL candidates: '+str(len(bundled)))
    print('Dependency report: '+str(reportpath))
    if missing:raise RuntimeError('Unresolved imports: '+', '.join(sorted({x['dll'] for x in missing})))
    if not a.stage:
        print('Audit only. Add --stage to copy the unpublished candidate.');return
    dest.mkdir()
    shutil.copy2(exe,dest/'mame.exe')
    for file in bundled.values():shutil.copy2(file,dest/file.name)
    docs=dest/'docs/video-preview';docs.mkdir(parents=True)
    shutil.copy2(reportpath,docs/'dependency-report.json')
    (docs/'RELEASE-STATUS.txt').write_text('UNPUBLISHED CANDIDATE: do not distribute yet.\nLicense notices, corresponding source, clean-system testing and final manifest/ZIP are still required.\nThis folder deliberately contains no replacement ui.ini or mame.ini and no media.\n',encoding='utf-8')
    print('Staged unpublished overlay candidate: '+str(dest))
if __name__=='__main__':
    try:main()
    except (OSError,RuntimeError) as e:raise SystemExit('ERROR: '+str(e))
