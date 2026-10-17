# convert a Linux /proc/asound codec dump (first Realtek codec) to a flat table
import re, sys
def amp(s):
    m = re.search(r'ofs=0x(\w+), nsteps=0x(\w+), stepsize=0x(\w+), mute=(\d)', s)
    if not m: return 0
    return int(m.group(1),16) | int(m.group(2),16) << 8 | int(m.group(3),16) << 16 | int(m.group(4)) << 31
lines = open(sys.argv[1], errors='replace').read().splitlines()
codecs = []; cur = None
for l in lines:
    if l.startswith('Codec:'):
        cur = [l]; codecs.append(cur)
    elif cur is not None:
        cur.append(l)
sec = None
for c in codecs:
    if 'Realtek' in c[0] or any(x.startswith('Vendor Id: 0x10ec') for x in c[:6]):
        sec = c; break
if sec is None: sys.exit('no realtek codec')
hdr = dict(vid=0, ssid=0, gpio=0, dai=0, dao=0, rates=0, afg=1)
nodes = []; n = None; mode = None
for l in sec:
    m = re.match(r'Node 0x(\w+) (?:\(\d+\) )?\[.*\] wcaps 0x(\w+)', l)
    if m:
        n = dict(nid=int(m.group(1),16), wcaps=int(m.group(2),16), pincap=0, cfg=0, ai=0, ao=0, rates=0, conn=[])
        nodes.append(n); mode = None; continue
    s = l.strip()
    if n is None:
        if s.startswith('Vendor Id:'): hdr['vid'] = int(s.split()[2],16)
        elif s.startswith('Subsystem Id:'): hdr['ssid'] = int(s.split()[2],16)
        elif s.startswith('AFG Function Id'): pass
        elif s.startswith('GPIO:'): hdr['gpio'] = int(re.search(r'io=(\d+)', s).group(1))
        elif s.startswith('Default Amp-In caps'): hdr['dai'] = amp(s)
        elif s.startswith('Default Amp-Out caps'): hdr['dao'] = amp(s)
        elif s.startswith('rates [') and not hdr['rates']:
            hdr['rates'] = int(re.search(r'\[0x(\w+)\]', s).group(1),16)
        elif s.startswith('bits [') and not (hdr['rates'] >> 16):
            hdr['rates'] |= int(re.search(r'\[0x(\w+)\]', s).group(1),16) << 16
        continue
    if s.startswith('Amp-In caps'): n['ai'] = amp(s)
    elif s.startswith('Amp-Out caps'): n['ao'] = amp(s)
    elif s.startswith('Pincap'): n['pincap'] = int(s.split()[1].rstrip(':'),16)
    elif s.startswith('Pin Default'): n['cfg'] = int(s.split()[2].rstrip(':'),16)
    elif s.startswith('rates [') and not n['rates']: n['rates'] = int(re.search(r'\[0x(\w+)\]', s).group(1),16)
    elif s.startswith('bits [') and not (n['rates'] >> 16): n['rates'] |= int(re.search(r'\[0x(\w+)\]', s).group(1),16) << 16
    elif s.startswith('Connection:'): mode = 'conn' if int(s.split()[1]) else None
    elif mode == 'conn':
        n['conn'] = [int(x.rstrip('*'),16) for x in s.split() if re.match(r'0x[0-9a-fA-F]+\*?$', x)]; mode = None
print('H %x %x %x %x %x %x %x %x' % (hdr['vid'], hdr['ssid'], nodes[0]['nid'], nodes[-1]['nid'] - nodes[0]['nid'] + 1,
      hdr['gpio'], hdr['dai'], hdr['dao'], hdr['rates']))
for n in nodes:
    print('N %x %x %x %x %x %x %x %d %s' % (n['nid'], n['wcaps'], n['pincap'], n['cfg'], n['ai'], n['ao'], n['rates'],
          len(n['conn']), ' '.join('%x' % c for c in n['conn'])))
