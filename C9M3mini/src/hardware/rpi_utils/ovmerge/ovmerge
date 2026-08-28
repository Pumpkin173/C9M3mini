#!/usr/bin/env python3

# Author: Phil Elwell <phil@raspberrypi.com>
# Copyright (c) 2018-2026, Raspberry Pi Ltd.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions, and the following disclaimer,
#    without modification.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. The names of the above-listed copyright holders may not be used
#    to endorse or promote products derived from this software without
#    specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
# IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
# LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
# NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Python port of the Perl ovmerge tool. CLI wrapper - the DT tokeniser,
# parser, and merge/apply engine live in ovmerge_engine.py.

import re
import sys

from ovmerge_engine import (
    S, OvMergeError, dtparse, dtdump, open_source_lines,
    get_prop_string, get_child, get_prop, set_prop, add_label, resolve_label,
    dtparam, get_node, ovapply1, get_fragments, delete_node, delete_prop,
    adj_ref, get_labels, ovstrip, renumber_fragments, ovmerge, ovapply2,
    is_node_empty, add_node, PROPS,
)


# Main
# ---------------------------------------------------------------------------

def usage():
    w = sys.stderr.write
    w("Usage: ovmerge <options> <ovspec>\n")
    w("  where <ovspec> is the name of an overlay, optionally followed by\n")
    w("    a comma-separated list of parameters, each with optional '=<value>'\n")
    w("    assignments. The presence of any parameters, or a comma followed by\n")
    w("    no parameters, removes the parameter declarations from the merged\n")
    w("    overlay to avoid a potential name clash.\n")
    w("  and <options> are any of:\n")
    w("    -b <branch>  Read files from specified git branch\n")
    w("    -B <token numer>  Set a debug breakpoint on the specified token number\n")
    w("    -c      Include 'redo' comment with command line (c.f. '-r')\n")
    w("    -e      Expand mode - list non-skipped lines in order of inclusion\n")
    w("    -f      Force some errors to be ignored\n")
    w("    -h      Display this help info\n")
    w("    -i      Show include hierarchy for each file\n")
    w("    -l      Like expand mode, but labels each line with source file\n")
    w("    -n      No .dts file header (just parsing .dtsi files)\n")
    w("    -N      Don't renumber overlay fragments (not guaranteed to work)\n")
    w("    -p      Emulate Pi firmware manipulation\n")
    w("    -q      Query mode (no output, just the success/failure return code)\n")
    w("    -r      Redo command comment in named files (c.f. '-c')\n")
    w("    -s      Sort nodes and properties (for easy comparison)\n")
    w("    -S <n>  Instead of tabs, use 'n' spaces for indentation\n")
    w("    -t      Trace the tree changes\n")
    w("    -T      Trace the parsing process\n")
    w("    -w      Show warnings\n")
    sys.exit(1)


def run(argv):
    args = list(argv)
    cmdline = []
    redo_comments = []

    while args and args[0].startswith('-') and args[0] != '-':
        arg = args.pop(0)

        if arg == '-b':
            if not args:
                print("* Branch parameter missing", file=sys.stderr)
                usage()
            S.branch = args.pop(0)
            cmdline += [arg, S.branch]
        elif arg == '-B':
            if not args:
                print("* Breakpoint number missing", file=sys.stderr)
                usage()
            S.bkpt = int(args.pop(0))
        elif arg == '-c':
            S.comment = True
        elif arg == '-e':
            S.expand = True
        elif arg == '-h':
            usage()
        elif arg == '-i':
            S.show_includes = True
        elif arg == '-l':
            S.expand = True
            S.expand_label = True
        elif arg == '-n':
            S.no_dts = True
        elif arg == '-p':
            S.pi_extras = True
            cmdline.append(arg)
        elif arg == '-r':
            if args:
                lines = open_source_lines(args[0])
            else:
                lines = sys.stdin.readlines()
            it = iter(lines)
            firstline = next(it, '')
            m = re.match(r'^// redo: ovmerge (.*)', firstline)
            if not m:
                print("* Redo but input has no 'redo:' comment", file=sys.stderr)
                usage()
            args = re.split(r'\s+', m.group(1).strip())
            for line in it:
                line = line.rstrip('\n')
                if re.match(r'^/dts-v1/', line):
                    break
                redo_comments.append(line)
        elif arg == '-s':
            S.sort = True
            cmdline.append(arg)
        elif arg == '-t':
            S.trace_tree = True
        elif arg == '-T':
            S.trace_parse = True
        elif arg == '-w':
            S.warnings = True
        elif arg == '-q':
            S.query = True
        elif arg == '-f':
            S.force = True
        elif arg == '-N':
            S.no_renumber = True
        elif arg == '-S':
            if not args:
                print("* Spaces count parameter missing", file=sys.stderr)
                usage()
            indent_spaces = args.pop(0)
            if not re.fullmatch(r'\d+', indent_spaces):
                print("* Invalid spaces count parameter. Expected an integer.", file=sys.stderr)
                usage()
            S.indent_str = ' ' * int(indent_spaces)
            cmdline += [arg, indent_spaces]
        else:
            print(f"* Unknown option '{arg}'", file=sys.stderr)
            usage()

    if not args:
        usage()

    cmdline += args

    overlays = []

    for overlay in args:
        if S.trace_tree:
            print(f"[ overlay {overlay} ]")

        m = re.match(r'^([^,:]+)', overlay)
        ovname = m.group(1) if m else ''
        overlay_rest = overlay[len(ovname):]
        dt = dtparse(ovname, S.no_dts)
        apply_params = overlay_rest.startswith(',')

        if S.show_includes or S.expand:
            continue

        model = get_prop_string(dt.root, 'model')

        S.cur_dt = dt

        if model and re.match(r'^Raspberry Pi', model) and S.pi_extras:
            aliases = get_child(dt.root, 'aliases')
            i2c = get_prop(aliases, 'i2c1')[1]
            set_prop(aliases, 'i2c', i2c)
            set_prop(aliases, 'i2c_arm', i2c)

            i2c_node = resolve_label(dt, i2c[1])
            add_label(dt, i2c_node, 'i2c_arm')

            i2c = get_prop(aliases, 'i2c0')[1]
            set_prop(aliases, 'i2c_vc', i2c)

            i2c_node = resolve_label(dt, i2c[1])
            add_label(dt, i2c_node, 'i2c_vc')

            overrides = get_child(dt.root, '__overrides__')
            i2c_prop = get_prop(overrides, 'i2c1')
            prop_rest = i2c_prop[1:]
            set_prop(overrides, 'i2c', *prop_rest)
            set_prop(overrides, 'i2c_arm', *prop_rest)

            i2c_prop = get_prop(overrides, 'i2c0')
            prop_rest = i2c_prop[1:]
            set_prop(overrides, 'i2c_vc', *prop_rest)

        if apply_params:
            seg_re = re.compile(r'[,:]([^=,]+)(?:=([^,]+))?')
            pos = 0
            while True:
                mm = seg_re.match(overlay_rest, pos)
                if not mm:
                    break
                pos = mm.end()
                dtparam(dt, mm.group(1), mm.group(2) if mm.group(2) is not None else '')
            if dt.plugin:
                ovapply1(dt)

        exports = get_node(dt, '/__exports__')
        if exports:
            for symbol in exports[PROPS]:
                expname = symbol[0]
                adj_ref(1, expname)

        if apply_params:
            delete_node(get_node(dt, '/__overrides__'))
            for fragment in get_fragments(dt):
                if get_child(fragment, '__dormant__'):
                    delete_node(fragment)

                payload = get_child(fragment, '__overlay__')
                if payload is not None and get_prop(payload, 'dtoverlay,preserve-phandle'):
                    for label in get_labels(payload):
                        adj_ref(1, label)
                    delete_prop(payload, 'dtoverlay,preserve-phandle')

        S.cur_dt = None

        if dt.plugin and not S.no_renumber:
            ovstrip(dt)

        overlays.append(dt)

    if not overlays:
        sys.exit(0)

    if overlays[0].plugin:
        if not S.no_renumber:
            renumber_fragments(overlays[0], 0)

        for i in range(1, len(overlays)):
            ovmerge(overlays[0], overlays[i])
    else:
        base = overlays[0]

        if len(overlays) > 1:
            symbols = get_child(base.root, '__symbols__')
            if symbols is None:
                symbols = add_node(base.root, '__symbols__')

            renumber_fragments(overlays[1], 0)

            for i in range(2, len(overlays)):
                ovmerge(overlays[1], overlays[i])

            ovapply2(base, overlays[1])

            if is_node_empty(symbols):
                delete_node(symbols)

    out = sys.stdout

    if S.comment:
        parts = ['// redo: ovmerge -c']
        for opt in cmdline:
            if re.search(r'\s', opt):
                parts.append(f" '{opt}'")
            else:
                parts.append(f" {opt}")
        out.write(''.join(parts) + "\n")
        if redo_comments:
            out.write("\n".join(redo_comments))
        out.write("\n")

    if not S.query:
        dtdump(overlays[0], out)

    sys.exit(S.retcode)


def main():
    try:
        run(sys.argv[1:])
    except OvMergeError as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
