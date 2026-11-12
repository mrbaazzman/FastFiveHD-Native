from pathlib import Path

text = Path(__file__).resolve().parent.parent.parent.joinpath("runtime/imports_table.h").read_text()
names = [line.split('"')[1] for line in text.splitlines() if line.count('"') >= 2]
gl = [n for n in names if n.startswith("gl")]
print(len(gl))
print("\n".join(gl))
