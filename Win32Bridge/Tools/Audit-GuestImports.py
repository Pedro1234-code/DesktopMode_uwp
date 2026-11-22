"""Read PE64 imports and audit source registrations. Not a linker/runtime test."""
import argparse,hashlib,json,re,struct
from pathlib import Path

def imports(path):
    data=path.read_bytes(); pe=struct.unpack_from('<I',data,60)[0];opt=pe+24
    if data[pe:pe+4]!=b'PE\0\0' or struct.unpack_from('<H',data,pe+4)[0]!=0x8664 or struct.unpack_from('<H',data,opt)[0]!=0x20b:
        raise ValueError('Expected x64 PE32+')
    sections=[]
    for i in range(struct.unpack_from('<H',data,pe+6)[0]):
        offset=opt+struct.unpack_from('<H',data,pe+20)[0]+40*i
        size,va,raw_size,raw=struct.unpack_from('<IIII',data,offset+8)
        sections.append((va,max(size,raw_size),raw))
    def at(rva):
        for va,size,raw in sections:
            if va<=rva<va+size:return raw+rva-va
        return rva
    def text(rva):return data[at(rva):].split(b'\0',1)[0].decode('ascii')
    result=[]
    for delay,index in [(False,1),(True,13)]:
        rva,size=struct.unpack_from('<II',data,opt+112+index*8)
        if not rva:continue
        offset=at(rva)
        while True:
            if delay:
                fields=struct.unpack_from('<8I',data,offset);offset+=32
                if not any(fields):break
                if not fields[0]&1:raise ValueError('VA-based delay descriptors are unsupported')
                name,iat,lookup=fields[1],fields[3],fields[4]
            else:
                fields=struct.unpack_from('<5I',data,offset);offset+=20
                if not any(fields):break
                lookup,name,iat=fields[0],fields[3],fields[4];lookup=lookup or iat
            i=0
            while True:
                thunk=struct.unpack_from('<Q',data,at(lookup)+i*8)[0]
                if not thunk:break
                symbol='#'+str(thunk&65535) if thunk>>63 else text(thunk+2)
                result.append({'library':text(name),'name':symbol,'delay':delay});i+=1
    return result

def canonical(library):
    name=library.replace('/','\\').split('\\')[-1].lower()
    if name.startswith('api-ms-win-crt-'):return 'ucrtbase.dll'
    for prefix,host in [('ext-ms-win-cmd-util-','cmdext.dll'),('ext-ms-win-branding-winbrand-','winbrand.dll'),('ext-ms-win-appmodel-shellexecute-','shell32.dll'),('api-ms-win-core-winrt-','combase.dll')]:
        if name.startswith(prefix):return host
    if not name.startswith(('api-ms-win-','ext-ms-win-')):return name
    for token,host in [('-ntuser-','user32.dll'),('-gdi-','gdi32.dll'),('-shell-','shell32.dll'),('-shcore-','shcore.dll'),('-com-','ole32.dll'),('-ole-','ole32.dll'),('-security-','advapi32.dll'),('-service-','advapi32.dll'),('-eventing-','advapi32.dll'),('-registry-','advapi32.dll'),('-advapi32-','advapi32.dll'),('-version-','version.dll'),('-commctrl-','comctl32.dll'),('-comdlg-','comdlg32.dll')]:
        if token in name:return host
    if name.startswith(('api-ms-win-core-','api-ms-win-downlevel-kernel','api-ms-win-appmodel-')):return 'kernel32.dll'
    if name.startswith(('api-ms-win-ro-','api-ms-win-rtcore-')):return 'combase.dll'
    return name

def registered(text):
    names=set(re.findall(r'\bsymbol\.name\s*==\s*L"([^"]+)"',text))
    # Exception helper lookup uses its own `name` parameter instead of symbol.name.
    names.update(re.findall(r'std::wcscmp\(name,\s*L"([^"]+)"',text))
    names.update(re.findall(r'\bName\(symbol\.name,\s*L"([^"]+)"',text))
    names.update(re.findall(r'\{\s*L"([^"]+)"\s*,\s*reinterpret_cast',text))
    # Named resolver macros may generate narrow and wide variants from one call.
    for line in text.splitlines():
        if re.match(r'\s*(?:CRT_\w+|KERNEL_EXTRA|NATIVE_EXPORT)\(',line):
            names.update(re.findall(r'"([^"]+)"',line))
    insensitive=set(re.findall(r'_wcsicmp\(symbol\.name\.c_str\(\),\s*L"([^"]+)"',text))
    return names,{name.lower() for name in insensitive}

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('pe',type=Path);parser.add_argument('--output',type=Path);args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]/'Bridge'
    groups={
      'kernel32.dll':['Kernel32Shims.cpp','Kernel32CoreShims.inl','ExtendedKernelShims.inl','ConsoleShims.inl','PipeShims.inl','EnvironmentShims.inl'],
      'ntdll.dll':['Kernel32Shims.cpp','Kernel32CoreShims.inl','NativeGuestShims.inl'],
      'advapi32.dll':['Advapi32Shims.cpp','NativeGuestShims.inl','OptionalContracts.inl'],
      'ucrtbase.dll':['CRT/MsvcrtShims.cpp','CRT/CrtExceptions.cpp','CRT/CrtDataExports.inl','CRT/CrtStartup.inl','CRT/CrtFileShims.inl','CRT/CrtStdioShims.inl','CRT/CrtFormattingShims.inl','CRT/CrtScanningShims.inl','CRT/CrtConsoleProcess.inl','CRT/CrtHeapShims.inl','CRT/CrtLocaleExports.inl'],
      'combase.dll':['OleShims.cpp'], 'ole32.dll':['OleShims.cpp'],
      'shell32.dll':['Shell32Shims.cpp'], 'cmdext.dll':['OptionalContracts.inl'],
      'msvcp140.dll':['CRT/CrtExceptionPtr.inl'], 'msvcp140_app.dll':['CRT/CrtExceptionPtr.inl'],
      'winbrand.dll':['OptionalContracts.inl'], 'wldp.dll':['OptionalContracts.inl'],
      'user32.dll':['User32Shims.cpp','User32CompatibilityShims.inl','Win32Shims.cpp'],
      'gdi32.dll':['GDI/Gdi32Shims.cpp','GDI/Gdi32CompatibilityShims.inl'],
      'mpr.dll':['MprShims.cpp']}
    inventory={key:registered('\n'.join((root/name).read_text(encoding='utf-8-sig') for name in files)) for key,files in groups.items()}
    inventory['kernelbase.dll']=inventory['kernel32.dll'];inventory['msvcrt.dll']=inventory['ucrtbase.dll']
    redirects={}
    for dll,name,ordinal,host,target,target_ordinal in re.findall(r'\{ L"([^"]+)", L"([^"]*)", (\d+), L"([^"]+)", L"([^"]*)", (\d+) \}',(root/'GeneratedExportAliases.inl').read_text(encoding='utf-8-sig')):
        redirects[(dll.lower(),'#'+ordinal if int(ordinal) else name)]=(host,'#'+target_ordinal if int(target_ordinal) else target)
    def covered(library,name,visited=()):
        library=canonical(library);key=(library,name)
        if key in visited or len(visited)>=32:return False
        if library=='wldp.dll' and name in ['#2','#10']:return True
        known,insensitive=inventory.get(library,(set(),set()))
        if name in known or name.lower() in insensitive:return True
        if library=='ucrtbase.dll' and name.startswith('_o_') and not name[3:].startswith('_o_') and len(name)>3:
            return covered(library,name[3:],visited+(key,))
        target=redirects.get(key)
        return covered(*target,visited+(key,)) if target else False
    rows=imports(args.pe)
    for row in rows:row['source_registration_candidate']=covered(row['library'],row['name'])
    missing=[row for row in rows if not row['source_registration_candidate']]
    report={'pe':str(args.pe),'sha256':hashlib.sha256(args.pe.read_bytes()).hexdigest(),'verification':'Static source-registration audit only: no C++ build, address resolution or guest execution. Conditional DLL guards and native linking require runtime validation.','regular':sum(not row['delay'] for row in rows),'delay':sum(row['delay'] for row in rows),'uncovered':missing,'imports':rows}
    if args.output:args.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(f"Static audit: {report['regular']} regular; {report['delay']} delay; {len(missing)} symbol names without source-registration candidates.")
    for row in missing:print(row['library']+'!'+row['name'])
    raise SystemExit(bool(missing))

if __name__=='__main__':main()
