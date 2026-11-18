from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
source = (root / "src" / "prism_sync.cpp").read_text(encoding="utf-8")
match = re.search(r'const char kShaders\[\] = R"\((.*?)\)";', source, re.DOTALL)
if match is None:
    raise SystemExit("prism_sync shader source not found")
out = root / "build" / "prism_sync_validate.hlsl"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(match.group(1), encoding="utf-8")
print(out)
