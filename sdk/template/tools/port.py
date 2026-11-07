"""Bergamota's port tool (sdk/port.py) for this port: build, play and
package it (Bergamota's docs/porting.md).

    python -I tools/port.py check | symbols | native | build | play | boot | package [...]

The runtime is RUNTIME if set, else a checkout of Bergamota two folders up
in tools/runtime (a workspace with the ports in ports/), else the
external/runtime submodule.
"""

import os
import runpy
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
for runtime in (os.environ.get("RUNTIME"), PORT.parents[1] / "tools" / "runtime", PORT / "external" / "runtime"):
    if runtime and (Path(runtime) / "sdk" / "port.py").exists():
        break
else:
    sys.exit("no Bergamota with sdk/: git submodule update --init (external/runtime), or RUNTIME=<checkout>")
if "--port" not in sys.argv:
    sys.argv[2:2] = ["--port", str(PORT)] if len(sys.argv) > 1 else []
sys.argv[0] = str(Path(runtime) / "sdk" / "port.py")
runpy.run_path(sys.argv[0], run_name="__main__")
