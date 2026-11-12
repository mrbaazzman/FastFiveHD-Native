import re
from pathlib import Path

txt = Path(__file__).resolve().parent.parent.parent.joinpath("runtime/imports_table.h").read_text()
names = re.findall(r'"([^"]+)"', txt)
keys = ("open", "read", "write", "stat", "dir", "file", "close", "seek", "tell",
        "remove", "rename", "unlink", "access", "fget", "fput", "fprintf", "fscanf")
for n in names:
    if any(k in n for k in keys):
        print(n)
