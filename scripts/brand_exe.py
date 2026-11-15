"""brand_exe.py [game_dir] [--icon a.png [b.png ...]]

Creates <game_dir>/onrush-pc.exe from <game_dir>/game_release_xbox.exe (the source is never modified)
and rewrites its version resource (FileDescription / ProductName / OriginalFilename) so Task Manager
shows "ONRUSH(TM)" instead of "evo-x". With --icon, the given PNG(s) become the executable icon
(the largest image is resized to 16..256 px; a smaller image, if given, is used for the small sizes).
"""
import sys, os, shutil, struct, ctypes, io
from ctypes import wintypes

GAME_DIR = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else \
    os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'dist_game')
SRC = os.path.join(GAME_DIR, 'game_release_xbox.exe')
DST = os.path.join(GAME_DIR, 'onrush-pc.exe')
NAME = 'ONRUSH™'

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]; k32.BeginUpdateResourceW.restype = wintypes.HANDLE
k32.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD, ctypes.c_void_p, wintypes.DWORD]
k32.UpdateResourceW.restype = wintypes.BOOL
k32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]; k32.EndUpdateResourceW.restype = wintypes.BOOL
RT_ICON, RT_GROUP_ICON, RT_VERSION = 3, 14, 16

def _pad(b):
    return b + b'\0' * ((4 - len(b) % 4) % 4)

def _node(key, value=b'', vtype=1, children=b'', value_len=None):
    """VS_VERSIONINFO-style block: wLength, wValueLength, wType, szKey, padding, Value, padding, Children."""
    k = _pad(struct.pack('<HHH', 0, 0, 0) + (key + '\0').encode('utf-16le'))[6:]
    body = k + _pad(value) + children
    vlen = value_len if value_len is not None else (len(value) // 2 if vtype == 1 else len(value))
    return _pad(struct.pack('<HHH', 6 + len(body), vlen, vtype) + body)

def version_resource(strings):
    ffi = struct.pack('<13I', 0xFEEF04BD, 0x00010000, 0x00010000, 0x00000001, 0x00010000, 0x00000001,
                      0x3F, 0, 0x40004, 1, 0, 0, 0)
    tbl = b''.join(_node(k, (v + '\0').encode('utf-16le')) for k, v in strings.items())
    sfi = _node('StringFileInfo', vtype=1, children=_node('041604B0', vtype=1, children=tbl), value_len=0)
    vfi = _node('VarFileInfo', vtype=1, children=_node('Translation', struct.pack('<HH', 0x0416, 0x04B0), vtype=0), value_len=0)
    return _node('VS_VERSION_INFO', ffi, vtype=0, children=sfi + vfi)

def icon_images(pngs):
    from PIL import Image
    ims = [Image.open(p).convert('RGBA') for p in pngs]
    ims.sort(key=lambda im: im.width)
    out = []
    for s in (16, 24, 32, 48, 64, 128, 256):
        srcim = next((im for im in ims if im.width >= s), ims[-1]) if s <= 64 else ims[-1]
        im = srcim.resize((s, s), Image.LANCZOS)
        buf = io.BytesIO(); im.save(buf, 'PNG'); out.append((s, buf.getvalue()))
    return out

def main():
    pngs = sys.argv[sys.argv.index('--icon') + 1:] if '--icon' in sys.argv else []
    if not os.path.exists(SRC):
        sys.exit('missing ' + SRC)
    shutil.copy2(SRC, DST)
    h = k32.BeginUpdateResourceW(DST, False)
    if not h:
        sys.exit('BeginUpdateResource failed %d' % ctypes.get_last_error())
    ver = version_resource({'CompanyName': 'Codemasters', 'FileDescription': NAME, 'FileVersion': '1.0.0.1',
                            'InternalName': 'onrush-pc', 'OriginalFilename': 'onrush-pc.exe',
                            'ProductName': NAME, 'ProductVersion': '1.0.0.1'})
    # The game ships one version block (lang 0x809, en-GB). Deleting a language that does not exist
    # makes EndUpdateResource fail (1359), so replace exactly that entry.
    ok = k32.UpdateResourceW(h, RT_VERSION, 1, 0x0809, ver, len(ver))
    if pngs:
        imgs = icon_images(pngs)
        grp = struct.pack('<HHH', 0, 1, len(imgs))
        for i, (s, data) in enumerate(imgs, start=1):
            k32.UpdateResourceW(h, RT_ICON, i, 0x0809, data, len(data))
            grp += struct.pack('<BBBBHHIH', s % 256, s % 256, 0, 0, 1, 32, len(data), i)
        k32.UpdateResourceW(h, RT_GROUP_ICON, 1, 0x0809, grp, len(grp))
    if not k32.EndUpdateResourceW(h, False) or not ok:
        sys.exit('resource update failed %d' % ctypes.get_last_error())
    print('wrote', DST, '(icon: %s)' % ('yes' if pngs else 'no'))
    if pngs:
        # same icon on the launcher, and the splash art next to it (the launcher shows it)
        launcher = os.path.join(GAME_DIR, 'RushLauncher.exe')
        if os.path.exists(launcher):
            hl = k32.BeginUpdateResourceW(launcher, False)
            imgs = icon_images(pngs)
            grp = struct.pack('<HHH', 0, 1, len(imgs))
            for i, (s, data) in enumerate(imgs, start=1):
                k32.UpdateResourceW(hl, RT_ICON, i, 0x0409, data, len(data))
                grp += struct.pack('<BBBBHHIH', s % 256, s % 256, 0, 0, 1, 32, len(data), i)
            k32.UpdateResourceW(hl, RT_GROUP_ICON, 1, 0x0409, grp, len(grp))
            print('launcher icon:', bool(k32.EndUpdateResourceW(hl, False)))
        splash = os.path.join(os.path.dirname(os.path.abspath(pngs[0])), 'SplashScreen.png')
        if os.path.exists(splash):
            shutil.copy2(splash, os.path.join(GAME_DIR, 'SplashScreen.png'))
            print('copied', splash)

if __name__ == '__main__':
    main()
