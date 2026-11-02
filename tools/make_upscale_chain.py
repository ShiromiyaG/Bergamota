"""A chaiNNer chain (.chn) that upscales a folder of textures (texdump's, or
the player's texture-dump) with a PyTorch model and saves them under the
same names and folders, ready for the player's textures/ folder.

    python -I make_upscale_chain.py <textures folder> <out folder> <chain.chn> [model .pth/.safetensors]

Made for chaiNNer 0.26 (its save format: the content, an MD5 of it, and the
count of its migrations). Load Images goes through the folders too; the
padding is Wrap (Tile), as most of a game's textures repeat.
"""

import hashlib
import json
import sys
import uuid
from datetime import datetime, timezone

VERSION = "0.26.1"
MIGRATION = 41  # chaiNNer 0.26.1's migrations: a chain at that count is not migrated again


def node(schema, kind, x, y, inputs):
    nid = str(uuid.uuid4())
    return nid, {
        "data": {
            "schemaId": schema,
            "inputData": inputs,
            "id": nid,
        },
        "id": nid,
        "position": {"x": x, "y": y},
        "type": kind,
        "selected": False,
    }


def edge(source, output, target, input_id):
    return {
        "id": str(uuid.uuid4()),
        "sourceHandle": f"{source}-{output}",
        "targetHandle": f"{target}-{input_id}",
        "source": source,
        "target": target,
        "type": "main",
        "animated": False,
        "data": {},
    }


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    textures, out, chain = sys.argv[1], sys.argv[2], sys.argv[3]
    model = sys.argv[4] if len(sys.argv) > 4 else ""

    # Load Images: the folder (0), no glob (1), recursive (2), no limit (4), not stopping on errors (6)
    load, load_node = node("chainner:image:load_images", "newIterator", 0, 0,
                           {"0": textures, "1": 0, "2": 1, "3": "**/*", "4": 0, "5": 10, "6": 0})
    # Load Model: the .pth or .safetensors (0)
    model_id, model_node = node("chainner:pytorch:load_model", "regularNode", 0, 420, {"0": model} if model else {})
    # Upscale Image: tiles estimated (2), the model's own scale (4, 5), Wrap (Tile) padding (7), alpha with the color (3)
    up, up_node = node("chainner:pytorch:upscale_image", "regularNode", 420, 120,
                       {"2": 0, "3": 0, "4": 0, "5": 4, "6": 256, "7": 2})
    # Save Image: the out folder (1), the texture's subfolder (2) and name (3), PNG (4) at 8 bits (15)
    save, save_node = node("chainner:image:save", "regularNode", 820, 120,
                           {"1": out, "4": "png", "15": "u8"})

    content = {
        "nodes": [load_node, model_node, up_node, save_node],
        "edges": [
            edge(load, 0, up, 1),       # the image
            edge(model_id, 0, up, 0),   # the model
            edge(up, 0, save, 0),       # upscaled
            edge(load, 2, save, 2),     # its subfolder
            edge(load, 3, save, 3),     # its name: the hash the player looks for
        ],
        "viewport": {"x": 40, "y": 40, "zoom": 1},
    }
    text = json.dumps(content, separators=(",", ":"), ensure_ascii=False)
    saved = {
        "version": VERSION,
        "content": content,
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"),
        "checksum": hashlib.md5(text.encode("utf-8")).hexdigest(),
        "migration": MIGRATION,
    }
    with open(chain, "w", encoding="utf-8") as f:
        f.write(json.dumps(saved, separators=(",", ":"), ensure_ascii=False))
    print(chain)


if __name__ == "__main__":
    main()
