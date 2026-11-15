#!/usr/bin/env python3
"""Fetch the official submission template once, preserving provenance."""
import hashlib
import io
import json
from datetime import datetime, timezone
from pathlib import Path
from urllib.request import urlopen
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
BASE = "https://www.iscaconf.org/isca2026/submit/"
VENDOR = ROOT / "vendor"


def main():
    manifest = VENDOR / "source-manifest.json"
    if manifest.exists():
        data = json.loads(manifest.read_text())
        for name, checksum in data["files_sha256"].items():
            path = ROOT / name
            if hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
                raise SystemExit(f"Template verification failed: {path}")
        print("Existing official template verified; no files overwritten.")
        return
    if (VENDOR / "official").exists() or (ROOT / "IEEEtran.cls").exists():
        raise SystemExit("Unrecorded template files exist; inspect them before downloading.")
    downloads = {}
    for name in ("ISCA2026_IEEE_template.zip", "guidelines.php"):
        with urlopen(BASE + name, timeout=60) as response:
            downloads[name] = response.read()
    files = {"vendor/guidelines.html": downloads["guidelines.php"]}
    with ZipFile(io.BytesIO(downloads["ISCA2026_IEEE_template.zip"])) as archive:
        for member in archive.infolist():
            path = Path(member.filename)
            if member.is_dir() or "__MACOSX" in path.parts or path.name.startswith("."):
                continue
            if path.is_absolute() or ".." in path.parts:
                raise SystemExit("Unsafe archive path")
            target = "vendor/official/" + path.name
            if target in files:
                raise SystemExit(f"Duplicate template filename: {path.name}")
            files[target] = archive.read(member)
    files["IEEEtran.cls"] = files["vendor/official/IEEEtran.cls"]
    VENDOR.mkdir(parents=True, exist_ok=True)
    (VENDOR / "ISCA2026_IEEE_template.zip").write_bytes(downloads["ISCA2026_IEEE_template.zip"])
    for name, content in files.items():
        path = ROOT / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    data = {
        "retrieved_at_utc": datetime.now(timezone.utc).isoformat(),
        "template_url": BASE + "ISCA2026_IEEE_template.zip",
        "guidelines_url": BASE + "guidelines.php",
        "archive_sha256": hashlib.sha256(downloads["ISCA2026_IEEE_template.zip"]).hexdigest(),
        "files_sha256": {name: hashlib.sha256(content).hexdigest() for name, content in files.items()},
    }
    manifest.write_text(json.dumps(data, indent=2) + "\n")
    print("Downloaded official ISCA 2026 template and recorded SHA256 provenance.")


if __name__ == "__main__":
    main()
