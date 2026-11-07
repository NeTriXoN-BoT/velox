from pathlib import Path
import argparse
import struct, zipfile
args=argparse.ArgumentParser()
args.add_argument('--launcher',required=True)
args=args.parse_args()
p=Path(__file__).parent
with zipfile.ZipFile(args.launcher) as z:
    data=bytearray(z.read('res/layout/taskbar.xml'))
original=bytes(data)
resource_ids=[]
orientation_index=None
at=8
while at < len(data):
    kind,header,size=struct.unpack_from('<HHI',data,at)
    if kind==0x180:
        resource_ids=list(struct.unpack_from('<'+'I'*((size-header)//4),data,at+header))
        orientation_index=resource_ids.index(0x010100c4)
        resource_ids[orientation_index]=0x010103b2
        struct.pack_into('<I',data,at+header+4*orientation_index,0x010103b2)
    at+=size
assert orientation_index is not None
patched=0
at=8
while at < len(data):
    kind,header,size=struct.unpack_from('<HHI',data,at)
    if kind==0x102:
        start,stride,count=struct.unpack_from('<HHH',data,at+24)
        attributes=[]
        found=False
        for i in range(count):
            a=at+16+start+i*stride
            chunk=bytearray(data[a:a+stride])
            name=struct.unpack_from('<I',chunk,4)[0]
            if name==orientation_index:
                assert struct.unpack_from('<I',chunk,16)[0]==0
                struct.pack_into('<I',chunk,16,1)
                found=True
                patched+=1
            attributes.append((i,resource_ids[name] if name<len(resource_ids) else 0,chunk))
        if found:
            attributes.sort(key=lambda a:a[1])
            for shortcut in (30,32,34):
                old=struct.unpack_from('<H',data,at+shortcut)[0]
                if old:
                    new=next(i+1 for i,a in enumerate(attributes) if a[0]+1==old)
                    struct.pack_into('<H',data,at+shortcut,new)
            for i,(_,_,chunk) in enumerate(attributes):
                a=at+16+start+i*stride
                data[a:a+stride]=chunk
    at+=size
assert patched==1
(p/'stock-taskbar.xml').write_bytes(original)
(p/'compiled'/'taskbar.xml').write_bytes(data)
print('Patched navigation container direction and preserved Android attribute ordering')
