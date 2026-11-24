"""Image atomics run on the Metal texture (MSL 3.1): SPIRV-Cross's pointer emulation (<image>_atomic) misses block-linear images, so check_msl refuses it."""
import re

_EMULATED = re.compile(r'\bspvImage2DAtomicCoord\b|device\s+atomic_u?int\s*\*\s*\w+_atomic\b')


# 64-bit image atomics (SURED.D.2D.MAX.U64): the translator writes two 32-bit atomics on the rg32ui image, low word then high; wide_atomics makes them one
# ulong atomic (Metal's ulong view of RG32Uint is little-endian; Apple GPU family 9). Any other access of that image fails the shader.
_WIDE_DECL = re.compile(r'\b(texture(?:2d|2d_array|3d))<uint, access::read_write>(\s+)(\w+_rg32ui)\b')
_WIDE_STMT = r'^([ \t]*)(?:uint _\d+ = )?([\w.]*?\b%s)\.atomic_fetch_(max|min)\((.*)\)(?:\.x)?;[ \t]*$'
_WIDE_QUERY = r'\.get_(?:width|height|depth|array_size)\(\)'


def _split_args(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        depth += {'(': 1, '[': 1, ')': -1, ']': -1}.get(ch, 0)
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    out.append(cur.strip())
    return out


def wide_atomics(source):
    if '_rg32ui' not in source:
        return source
    for m in list(_WIDE_DECL.finditer(source)):
        name = m.group(3)
        if not re.search(r'\b%s\.atomic_' % re.escape(name), source):
            continue
        lines = source.split('\n')
        stmt = re.compile(_WIDE_STMT % re.escape(name))
        out, i = [], 0
        while i < len(lines):
            a = stmt.match(lines[i])
            if not a:
                out.append(lines[i])
                i += 1
                continue
            b = stmt.match(lines[i + 1]) if i + 1 < len(lines) else None
            if not b or b.group(2) != a.group(2) or b.group(3) != a.group(3):
                raise ValueError(f'{name}: a 32-bit atomic on a 64-bit image that is not the translator\'s low / high pair')
            x, y = _split_args(a.group(4)), _split_args(b.group(4))
            if len(x) < 2 or x[:-1] != y[:-1]:
                raise ValueError(f'{name}: the two halves of a 64-bit atomic name different texels')
            out.append(f'{a.group(1)}{a.group(2)}.atomic_{a.group(3)}({", ".join(x[:-1])}, ulong4(as_type<ulong>(uint2({x[-1]}, {y[-1]}))));')
            i += 2
        source = '\n'.join(out)
        left = re.sub(r'\b%s(?:%s|\.atomic_(?:max|min)\()' % (re.escape(name), _WIDE_QUERY), '', source)
        uses = len(re.findall(r'\b%s\b' % re.escape(name), left))
        if uses != 1:   # the declaration alone
            raise ValueError(f'{name}: {uses - 1} accesses of a 64-bit atomic image besides its atomics and size queries')
        source = re.sub(r'\b(texture(?:2d|2d_array|3d))<uint, access::read_write>(\s+%s)\b' % re.escape(name), r'\1<ulong, access::read_write>\2', source)
    return source


def check_msl(source):
    if 'tomic' in source and _EMULATED.search(source):
        raise ValueError('image atomics emulated through a raw pointer; they must be native texture atomics (MSL 3.1)')
    return wide_atomics(source)
