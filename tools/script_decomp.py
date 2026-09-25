#!/usr/bin/env python3
"""Decompile a type 14 script into readable pseudo-code.

Operands are shown in the engine's own terms:
  gN   script global N          (raw < 0x13FE)
  LN   call-frame local N       (raw 0x13FE..0x159E; L0 is the first argument)
  #N   an immediate             (raw >= 0x159F, value raw + 0x7531)
Expressions (ops 76/77/78/79/89) are rebuilt as infix from the stack bytecode
of FUN_1008_1bf2. Script calls print as `script N(args)`, builtins as
`builtin_0xNN(args)`.

Usage:  script_decomp.py FILE.BIN INDEX
"""
import struct
import sys

import res_dir

BIN = {17: '+', 18: '-', 19: '*', 20: '/', 21: '%', 22: '==', 23: '<=', 24: '<', 25: '>=',
       26: '>', 27: '!=', 28: '&&', 29: '||', 30: '&', 31: '|', 32: '^'}

BUILTINS = {0x2E: 'random', 0x2F: 'key_down', 0x41: 'cos', 0x42: 'sin', 0x44: 'sprite_visible',
            0x5A: 'sprite_cmd_now', 0x5B: 'sprite_cmd_append', 0x62: 'abs', 0x65: 'clone_sprite',
            0x6B: 'set_layout_key', 0x79: 'long_add', 0x7A: 'long_mul', 0x7B: 'long_div',
            0x80: 'sqrt', 0x83: 'sprite_user', 0x84: 'set_sprite_user', 0x87: 'min', 0x88: 'max',
            0x8B: 'drive_type', 0x8C: 'free_mem'}

NAMES = {1: 'call', 4: 'hotspot', 5: 'place_sprite', 6: 'background', 8: 'sound', 12: 'bind_key',
         16: 'fold', 17: 'if_call', 18: 'load_scene', 19: 'set_timer', 20: 'kill_timer',
         21: 'post_script', 24: 'swap_sprite', 25: 'break_loops', 28: 'set_z', 29: 'focus_script',
         30: 'queue_call', 34: 'list_step', 36: 'move_by', 37: 'goto', 38: 'if_goto', 40: 'get_pos',
         43: 'switch_var', 44: 'end', 45: 'nop', 46: 'random_var', 47: 'key_state', 55: 'get_rect',
         56: 'sprintf', 59: 'ini_read', 60: 'ini_write', 63: 'set_pos', 64: 'mouse_filter',
         76: 'stmt', 77: 'if', 78: 'switch', 79: 'while', 83: 'bind_device', 84: 'input_script',
         89: 'return'}


def opnd(raw):
    if raw < 0x13FE:
        return 'g%d' % raw
    if raw < 0x159F:
        return 'L%d' % (raw - 0x13FE)
    v = (raw + 0x7531) & 0xFFFF
    return '#%d' % (v - 0x10000 if v >= 0x8000 else v)


def expr(b, p, n):
    """Rebuild the infix form of the expression at b[p:]. Returns (text, end)."""
    st = []
    while p < n:
        op = b[p]; p += 1
        if op == 0:
            break
        w = struct.unpack_from('<H', b, p)[0] if p + 2 <= n else 0
        if op == 1:
            st.append(str(w - 0x10000 if w >= 0x8000 else w)); p += 2
        elif op == 2:
            st.append(opnd(w)); p += 2
        elif op == 4:
            st.append('&' + opnd(w)); p += 2
        elif op in (12, 13):
            lo, argc = struct.unpack_from('<HH', b, p); p += 4
            args = st[len(st) - argc:] if argc else []
            if argc:
                del st[len(st) - argc:]
            tgt = st.pop() if st else '?'
            if op == 13:
                st.append('script %s(%s)' % (tgt, ', '.join(args)))
            else:
                try:
                    bid = int(tgt)
                    name = BUILTINS.get(bid, 'builtin_0x%02X' % bid)
                except ValueError:
                    name = 'builtin[%s]' % tgt
                st.append('%s(%s)' % (name, ', '.join(args)))
        elif op == 3:
            t = st.pop(); u = st.pop(); st.append('%s[%s]' % (u.lstrip('&') if u.startswith('&') else '*' + u, t))
        elif op == 5:
            st.append('*' + st.pop())
        elif op == 6:
            t = st.pop(); u = st.pop(); st.append('&%s[%s]' % (u.lstrip('&'), t))
        elif op == 8:
            t = st.pop(); u = st.pop(); st.append('%s = %s' % (u.lstrip('&') if u.startswith('&') else '*' + u, t))
        elif op == 14:
            st.append('-(%s)' % st.pop())
        elif op == 15:
            st.append('!(%s)' % st.pop())
        elif op == 16:
            st.append('~(%s)' % st.pop())
        elif op in BIN:
            t = st.pop(); u = st.pop(); st.append('(%s %s %s)' % (u, BIN[op], t))
        elif op in (33, 34, 35, 36):
            a = st.pop().lstrip('&')
            st.append({33: '++%s', 34: '%s++', 35: '--%s', 36: '%s--'}[op] % a)
        elif op in (37, 38):
            p += 2      # short-circuit jump: the combining || / && follows
        elif op == 39:
            st.append('*' + st[-1])
        else:
            st.append('<op%d>' % op)
    return (st[-1] if st else ''), p


def record_len(b, p):
    import script_dis
    r = script_dis.record_len(b, p)
    return r[0] if isinstance(r, tuple) else r


def decompile(b):
    import script_dis
    p, out, tables = 0, [], []
    while p + 2 <= len(b):
        skip = [t for t in tables if t[0] <= p < t[1]]
        if skip:                                   # inline switch table: data, not code
            out.append('%04x  [switch table, %d bytes]' % (p, skip[0][1] - p)); p = skip[0][1]; continue
        op = struct.unpack_from('<H', b, p)[0]
        w = [struct.unpack_from('<H', b, p + k)[0] if p + k + 2 <= len(b) else 0 for k in range(2, 20, 2)]
        name = NAMES.get(op, 'op%d' % op)
        line = None
        if op in (0, 44):
            out.append('%04x  end' % p); p += 2; continue
        if op == 76:
            e, _ = expr(b, p + 4, len(b)); line = e; ln = w[0]
        elif op == 89:
            e, end = expr(b, p + 4, len(b)); line = 'return ' + e; ln = end - p
        elif op in (77, 79):
            e, end = expr(b, p + 6, len(b))
            line = '%s %s  ? -> %04x : -> %04x' % (name, e, p + w[1], p + w[0]); ln = end - p
        elif op == 37:
            line = 'goto %04x' % (p + (w[0] - 0x10000 if w[0] >= 0x8000 else w[0])); ln = 4
            out.append('%04x  %s' % (p, line)); p += ln; continue
        elif op in (78, 43):
            if op == 78:
                e, _ = expr(b, p + 10, len(b))
            else:
                e = opnd(w[0])
            tbl = p + w[1]; cnt = w[3] & 0xFF
            tables.append((tbl, tbl + 6 * cnt))
            cases = []
            for k in range(cnt):
                q = tbl + k * 6
                if q + 6 <= len(b):
                    m, a = struct.unpack_from('<hH', b, q + 2)
                    cases.append('%d->%04x' % (m, p + a))
            line = 'switch %s { %s ; default->%04x }' % (e, ' '.join(cases), p + w[2])
        elif op == 1:
            frame, argc = b[p + 4], b[p + 5]
            args = [opnd(struct.unpack_from('<H', b, p + 6 + 2 * k)[0]) for k in range(argc)]
            line = 'call %s(%s)' % (opnd(w[0]), ', '.join(args))
        elif op == 18:
            line = 'load_scene "%s"' % b[p + 2:p + 16].split(b'\0')[0].decode('latin1')
        elif op == 5:
            line = 'place_sprite %s program %s%s' % (opnd(w[0]), opnd(w[1]) if w[1] else 'none', ' hide' if b[p + 6] else '')
        else:
            line = '%s %s' % (name, ' '.join(opnd(x) for x in w[:3]))
        try:
            ln = record_len(b, p)
        except Exception:
            ln = -1
        if ln is None or ln <= 0:
            out.append('%04x  %s   [length unknown, stopping]' % (p, line)); break
        out.append('%04x  %s' % (p, line))
        p += ln
    return out


if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    _, ents = res_dir.entries(data)
    e = ents[int(sys.argv[2])]
    b = res_dir.fetch(data, e)
    print('%s[%s] type %d, %d bytes' % (sys.argv[1].split('/')[-1], sys.argv[2], e['type'], len(b)))
    for l in decompile(b):
        print('  ' + l)
