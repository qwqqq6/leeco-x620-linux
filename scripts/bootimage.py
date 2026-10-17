"""Read/rebuild Android v0 boot images and newc initramfs without external tools."""
from __future__ import annotations
import gzip,hashlib,json,stat,struct,zlib
from pathlib import Path

def align(value,unit):return (value+unit-1)//unit*unit

def parse_boot(data):
    magic,ks,ka,rs,ra,ss,sa,ta,page,ds,version=struct.unpack_from('<8s10I',data)
    if magic!=b'ANDROID!' or page not in (2048,4096) or ss or ds or version:
        raise ValueError('Only this device\'s Android boot v0 layout is supported')
    offset=page+align(ks,page)
    return {'page_size':page,'kernel':data[page:page+ks],
            'ramdisk':data[offset:offset+rs],'header':data[:page]}

def parse_cpio(data):
    entries={};pos=0
    while pos+110<=len(data) and data[pos:pos+6] in (b'070701',b'070702'):
        fields=[int(data[pos+6+i*8:pos+14+i*8],16) for i in range(13)]
        name=data[pos+110:pos+109+fields[11]].decode()
        pos=align(pos+110+fields[11],4)
        content=data[pos:pos+fields[6]];pos=align(pos+fields[6],4)
        if name=='TRAILER!!!':break
        entries[name]={'mode':fields[1],'uid':fields[2],'gid':fields[3],'data':content,
                       'rdevmajor':fields[9],'rdevminor':fields[10]}
    return entries

def read_ramdisk(data):
    return parse_cpio(zlib.decompress(data,31))

def pack_cpio(entries):
    output=bytearray()
    for index,(name,row) in enumerate([*entries.items(),('TRAILER!!!',{'mode':0,'data':b''})],1):
        content=row.get('data',b'');rawname=name.encode()+b'\0'
        values=[index,row['mode'],row.get('uid',0),row.get('gid',0),1,0,len(content),0,0,
                row.get('rdevmajor',0),row.get('rdevminor',0),len(rawname),0]
        output.extend(b'070701'+b''.join(f'{value:08x}'.encode() for value in values)+rawname)
        output.extend(b'\0'*(align(len(output),4)-len(output)));output.extend(content)
        output.extend(b'\0'*(align(len(output),4)-len(output)))
    output.extend(b'\0'*(align(len(output),512)-len(output)))
    return bytes(output)

def rebuild(original,entries):
    layout=parse_boot(original);page=layout['page_size'];kernel=layout['kernel']
    ramdisk=gzip.compress(pack_cpio(entries),compresslevel=9,mtime=0)
    header=bytearray(layout['header']);struct.pack_into('<I',header,16,len(ramdisk))
    digest=hashlib.sha1()
    for blob in [kernel,ramdisk,b'']:
        digest.update(blob);digest.update(struct.pack('<I',len(blob)))
    header[576:608]=digest.digest()+b'\0'*12
    # Preserve all kernel bytes, including appended DTBs and the device's boot addresses.
    return bytes(header)+kernel+b'\0'*(align(len(kernel),page)-len(kernel))+ramdisk+b'\0'*(align(len(ramdisk),page)-len(ramdisk))
