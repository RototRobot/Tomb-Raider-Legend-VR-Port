# Scan the game's bigfiles for embedded DRMs (sector-aligned, version 14) and
# record every texture section's id -> (fmt, w, h, mips).
import sys,struct,mmap,glob,os
out={}; drms=0
for f in sorted(glob.glob(os.path.join(sys.argv[1],'bigfile.*'))):
    with open(f,'rb') as fh:
        m=mmap.mmap(fh.fileno(),0,access=mmap.ACCESS_READ); L=len(m)
        for base in range(0,L-8,2048):
            if m[base:base+4]!=b'\x0e\0\0\0': continue
            n=struct.unpack_from('<I',m,base+4)[0]
            if not 0<n<20000 or base+8+20*n>L: continue
            secs=[struct.unpack_from('<IBBHIII',m,base+8+20*i) for i in range(n)]
            if any(s[1]>20 for s in secs): continue
            off=base+8+20*n; ok=True; found=[]
            for (size,typ,pad,vid,packed,sid,mask) in secs:
                off+=(packed>>8)*8
                if off+size>L: ok=False;break
                if typ==5 and m[off:off+4]==b'PCD9':
                    fmt,dsz,unk,w,h,u8,mips,fl=struct.unpack_from('<4sIIHHBBH',m,off+4)
                    found.append((sid,fmt.hex(),w,h,mips,fl))
                off+=size
            if not ok: continue
            drms+=1
            for t in found: out[t[0]]=t[1:]
with open(sys.argv[2],'w') as o:
    for k in sorted(out): o.write('%d %s %d %d %d %d\n'%((k,)+out[k]))
print('drms',drms,'textures',len(out))
