#!/usr/bin/env python3
"""Extract the actual firmware HTML without redesigning the web interface."""
import ast
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/main.cpp').read_text()
for variable, name in [('loginIndex', 'login'), ('serverIndex', 'update')]:
    block = re.search(r'const char\* ' + variable + r'\s*=\s*(.*?);\s*\n', source, re.S).group(1)
    html = ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', block))
    output = ROOT / 'docs/preview' / (name + '.html')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(html + '\n')
    print(output.relative_to(ROOT))
