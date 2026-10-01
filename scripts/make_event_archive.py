"""R061: archive the per-packet and per-break event logs of the CARD-paper campaigns.

The main artefact (scripts/make_artifact.py) holds every per-run record; the published
bottleneck and mobility packet logs are in scr-adhoc-networks-packet-logs.zip. This builds
the companion archive for the CARD paper's campaigns (card_campaign, card_baselines,
card_taaodv, card_cardclaf, card_fading): every out_packet_events.csv and out_card_breaks.csv
of a completed run, each verified against the SHA-256 recorded in that run's DONE.json when
it finished, plus a manifest of paths and hashes.

Writes artifact/scr-adhoc-networks-card-event-logs.zip (+ .sha256).
Usage: python scripts/make_event_archive.py
"""

import hashlib
import json
import pathlib
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
CAMPAIGNS = ["card_campaign", "card_baselines", "card_taaodv", "card_cardclaf", "card_fading"]
LOGS = ["out_packet_events.csv", "out_card_breaks.csv"]
OUT = ROOT / "artifact" / "scr-adhoc-networks-card-event-logs.zip"


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    manifest, bad = [], []
    tmp = OUT.with_suffix(".zip.part")
    with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for camp in CAMPAIGNS:
            for done in sorted((ROOT / "checkpoints" / camp).glob("*/DONE.json")):
                d = done.parent
                rec = json.loads(done.read_text())
                for name in LOGS:
                    f = d / name
                    if not f.exists():
                        continue
                    h = sha(f)
                    want = rec.get("outputs", {}).get(name)
                    if want != h:
                        bad.append("%s/%s/%s" % (camp, d.name, name))
                        continue
                    arc = "%s/%s/%s" % (camp, d.name, name)
                    z.write(f, arc)
                    manifest.append({"path": arc, "sha256": h, "bytes": f.stat().st_size})
        z.writestr("MANIFEST.json", json.dumps({
            "description": "Per-packet and per-break event logs of the CARD paper's campaigns; each file's "
                           "SHA-256 equals the hash recorded in its run's DONE.json at completion.",
            "campaigns": CAMPAIGNS, "files": manifest}, indent=1))
    if bad:
        tmp.unlink()
        print("HASH MISMATCH, archive not written:", bad[:10])
        return 1
    tmp.replace(OUT)
    digest = sha(OUT)
    (OUT.parent / (OUT.name + ".sha256")).write_text("%s  %s\n" % (digest, OUT.name))
    print("wrote %s: %d files, %.1f MB, sha256 %s" % (OUT.name, len(manifest),
                                                     OUT.stat().st_size / 1e6, digest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
