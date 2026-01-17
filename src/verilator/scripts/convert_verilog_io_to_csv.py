#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
convert_verilog_io_to_csv.py

Convert a Verilog-like snippet containing write_io / repeat / for constructs
into CSV IO/INFO lines, expanding loops. Supports Verilog literals like
8'hFF and 'h40000, decimal, hex (0x...), expressions, & operator, etc.

This updated version accepts several increment syntaxes in for-loops:
 - i = i + N
 - i += N
 - i++
and will expand the loop accordingly (or emit a compact C loop if --emit-c
is provided and the iteration count is very large).

Usage:
  ./convert_verilog_io_to_csv.py [--emit-c] [inputfile] > out.csv
  cat input.txt | ./convert_verilog_io_to_csv.py --emit-c - > out.c
"""
from __future__ import annotations
import sys
import re
import ast
from typing import List

PORT_MAP = {
    'vdp_io0': 0x88,
    'vdp_io1': 0x89,
    'vdp_io2': 0x8A,
    'vdp_io3': 0x8B,
}

# ---------------------------
# Expression evaluator
# ---------------------------
class EvalExpr(ast.NodeVisitor):
    def __init__(self, names=None):
        self.names = names or {}

    def visit(self, node):
        if isinstance(node, ast.Expression):
            return self.visit(node.body)
        return super().visit(node)

    def generic_visit(self, node):
        raise ValueError(f"unsupported expression element: {type(node).__name__}")

    def visit_BinOp(self, node):
        left = self.visit(node.left)
        right = self.visit(node.right)
        op = node.op
        if isinstance(op, ast.Add):
            return left + right
        if isinstance(op, ast.Sub):
            return left - right
        if isinstance(op, ast.Mult):
            return left * right
        if isinstance(op, ast.Div):
            return left // right
        if isinstance(op, ast.Mod):
            return left % right
        if isinstance(op, ast.LShift):
            return left << right
        if isinstance(op, ast.RShift):
            return left >> right
        if isinstance(op, ast.BitOr):
            return left | right
        if isinstance(op, ast.BitAnd):
            return left & right
        if isinstance(op, ast.BitXor):
            return left ^ right
        raise ValueError(f"unsupported binary op: {op}")

    def visit_UnaryOp(self, node):
        operand = self.visit(node.operand)
        if isinstance(node.op, ast.USub):
            return -operand
        if isinstance(node.op, ast.UAdd):
            return +operand
        if isinstance(node.op, ast.Invert):
            return ~operand
        raise ValueError(f"unsupported unary op: {node.op}")

    def visit_Num(self, node):
        return node.n

    def visit_Constant(self, node):  # for py3.8+
        if isinstance(node.value, int):
            return node.value
        raise ValueError("unsupported constant type")

    def visit_Name(self, node):
        if node.id in self.names:
            return int(self.names[node.id])
        raise ValueError(f"unknown name: {node.id}")

def preprocess_verilog_literals(s: str) -> str:
    # Convert verilog literals:
    #  - 8'hFF or 16'h1A -> 0xFF / 0x1A
    #  - 8'd25 or 'd25 -> 25
    #  - 'h40000 -> 0x40000
    def rep_h(m):
        hexpart = m.group(2).replace("_", "")
        return hex(int(hexpart, 16))
    def rep_d(m):
        decpart = m.group(2).replace("_", "")
        return str(int(decpart, 10))
    # handle forms with width and without width
    s = re.sub(r"\b(\d+)'h([0-9A-Fa-f_]+)\b", rep_h, s)
    s = re.sub(r"\b(\d+)'d([0-9_]+)\b", rep_d, s)
    s = re.sub(r"\'h([0-9A-Fa-f_]+)\b", lambda m: hex(int(m.group(1).replace("_",""), 16)), s)
    s = re.sub(r"\'d([0-9_]+)\b", lambda m: str(int(m.group(1).replace("_",""), 10)), s)
    # remove trailing semicolons (we treat them separately)
    s = s.replace(";", " ")
    return s

def safe_eval(expr: str, names=None) -> int:
    s = preprocess_verilog_literals(expr)
    s = s.strip()
    if s == "":
        return 0
    try:
        node = ast.parse(s, mode="eval")
        ev = EvalExpr(names=names)
        return int(ev.visit(node))
    except Exception as e:
        raise ValueError(f"failed to eval '{expr}' -> '{s}': {e}")

# ---------------------------
# CSV / C output helpers
# ---------------------------
def info_line(s: str) -> str:
    s2 = s.replace('"', '""')
    return f'INFO,"{s2}"'

def io_line(port: str, value: int) -> str:
    p = port.strip()
    if p in PORT_MAP:
        portnum = PORT_MAP[p]
    else:
        portnum = int(p, 0)
    return f"IO,0x{portnum:02x},0x{(value & 0xFF):02x}"

def c_loop_for_write(port: str, start: int, end: int, step: int, expr: str) -> List[str]:
    portnum = PORT_MAP.get(port, int(port,0))
    lines = []
    lines.append(f"// Generated C loop for {port} : {start:#x} .. {end:#x} step {step}")
    lines.append(f"for (uint32_t i = {start:#x}; i < {end:#x}; i += {step}) {{")
    body_expr = preprocess_verilog_literals(expr)
    lines.append(f"    uint8_t v = (uint8_t)({body_expr} & 0xff);")
    lines.append(f"    vdp_cartridge_write_io(0x{portnum:02x}, v);")
    lines.append("}")
    return lines

# ---------------------------
# Parsing patterns
# ---------------------------
re_comment = re.compile(r'^\s*//\s*(.*)$')
re_write = re.compile(r'write_io\s*\(\s*(vdp_io[0-3]|0x[0-9A-Fa-f]+|\d+)\s*,\s*(.+?)\s*\)\s*;')
re_repeat_inline = re.compile(r'repeat\s*\(\s*(\d+)\s*\)\s*write_io\s*\(\s*(vdp_io[0-3]|0x[0-9A-Fa-f]+|\d+)\s*,\s*(.+?)\s*\)\s*;')
# for loop header: capture start, end, increment expression; inner block captured separately
re_for_header = re.compile(r'for\s*\(\s*i\s*=\s*(.+?)\s*;\s*i\s*<\s*(.+?)\s*;\s*(.+?)\s*\)\s*begin', re.S)

def parse_increment_expr(inc_expr: str) -> int:
    # Recognize patterns:
    #  i++      -> step = 1
    #  i += N   -> step = N
    #  i = i + N -> step = N
    s = inc_expr.strip()
    if s.endswith("++"):
        return 1
    m = re.match(r'i\s*\+=\s*(.+)', s)
    if m:
        return safe_eval(m.group(1))
    m = re.match(r'i\s*=\s*i\s*\+\s*(.+)', s)
    if m:
        return safe_eval(m.group(1))
    # fallback: try to evaluate the expression if it is an integer
    try:
        return safe_eval(s)
    except Exception:
        raise ValueError(f"unsupported increment expression: '{inc_expr}'")

def process_lines(lines: List[str], emit_c_for_large=False, expand_threshold=200000) -> List[str]:
    out: List[str] = []
    i = 0
    while i < len(lines):
        line = lines[i].rstrip('\n')
        i += 1
        if not line.strip():
            continue
        m = re_comment.match(line)
        if m:
            out.append(info_line(m.group(1).strip()))
            continue
        m = re_repeat_inline.match(line)
        if m:
            cnt = int(m.group(1))
            port = m.group(2)
            expr = m.group(3)
            val = safe_eval(expr)
            for _ in range(cnt):
                out.append(io_line(port, val))
            continue
        # detect for header possibly spanning lines
        if 'for' in line and 'begin' in line:
            block = line
            if 'end' not in line:
                # accumulate until 'end' is seen
                while i < len(lines):
                    block += '\n' + lines[i]
                    if 'end' in lines[i]:
                        i += 1
                        break
                    i += 1
            # parse header and body
            hm = re_for_header.search(block)
            if hm:
                start_expr = hm.group(1).strip()
                end_expr = hm.group(2).strip()
                inc_expr = hm.group(3).strip()
                # extract inner between 'begin' and 'end'
                inner_match = re.search(r'begin\s*(.*?)\s*end', block, re.S)
                inner = inner_match.group(1) if inner_match else ''
                wm = re_write.search(inner)
                if not wm:
                    out.append(info_line(block.strip()))
                    continue
                port = wm.group(1)
                body_expr = wm.group(2).strip()
                start_val = safe_eval(start_expr)
                end_val = safe_eval(end_expr)
                step_val = parse_increment_expr(inc_expr)
                if step_val == 0:
                    raise ValueError("for-loop increment evaluated to 0")
                # compute count
                if end_val > start_val and step_val > 0:
                    total_iters = (end_val - start_val + step_val - 1) // step_val
                else:
                    total_iters = 0
                if total_iters > expand_threshold and emit_c_for_large:
                    out.extend(c_loop_for_write(port, start_val, end_val, step_val, body_expr))
                    continue
                # expand
                for iv in range(start_val, end_val, step_val):
                    val = safe_eval(body_expr, names={'i': iv})
                    out.append(io_line(port, val))
                continue
            else:
                out.append(info_line(line.strip()))
                continue
        # fallback: inline write_io
        m = re_write.search(line)
        if m:
            port = m.group(1)
            expr = m.group(2)
            val = safe_eval(expr)
            out.append(io_line(port, val))
            continue
        # standalone repeat(...) on its own line
        m = re.search(r'repeat\s*\(\s*(\d+)\s*\)', line)
        if m:
            cnt = int(m.group(1))
            # next non-empty line should be write_io(...)
            while i < len(lines) and lines[i].strip() == "":
                i += 1
            if i < len(lines):
                nextline = lines[i].strip(); i += 1
                m2 = re_write.search(nextline)
                if m2:
                    port = m2.group(1)
                    expr = m2.group(2)
                    for _ in range(cnt):
                        val = safe_eval(expr)
                        out.append(io_line(port, val))
                    continue
                else:
                    out.append(info_line(line.strip()))
                    continue
        # unknown: emit INFO
        out.append(info_line(line.strip()))
    return out

# ---------------------------
# Main
# ---------------------------
def usage_and_exit():
    print("Usage: convert_verilog_io_to_csv.py [--emit-c] [inputfile]")
    print("  --emit-c    : for very large for-loops, emit compact C loop instead of expanding")
    sys.exit(2)

def main():
    emit_c = False
    args = sys.argv[1:]
    infile = None
    if args and args[0] in ("-h", "--help"):
        usage_and_exit()
    if args and args[0] == "--emit-c":
        emit_c = True
        args = args[1:]
    if len(args) > 1:
        usage_and_exit()
    if len(args) == 1:
        infile = args[0]

    if infile:
        with open(infile, 'r', encoding='utf-8') as f:
            lines = f.readlines()
    else:
        lines = sys.stdin.readlines()

    expanded = process_lines(lines, emit_c_for_large=emit_c, expand_threshold=200000)

    for l in expanded:
        print(l)

if __name__ == "__main__":
    main()

