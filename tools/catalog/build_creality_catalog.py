#!/usr/bin/env python3
"""Build a catalog of Creality's published firmware from the API behind
https://www.crealitycloud.com/downloads/firmware (no login needed).

  build_creality_catalog.py -o firmwares.yaml [--json firmwares.json]

How the site works (found by reading its scripts, 2026-11):
  POST https://admin.crealitycloud.com/api/cxy/v2/device/firmwarePrinterList {"isAll": true}
      -> every printer: name, internalName (e.g. F005), seriesId, hardwareVer, dbId
  POST https://admin.crealitycloud.com/api/cxy/search/firmwareSearch
      {"page": N, "pageSize": 100, "type": 2, "seriesId": "<id>"}   printer firmware of a series
      {"page": N, "pageSize": 100, "type": T}                       accessories (pads, boxes...), T != 2
      -> records: name (file), versionNumber, hardwareVersionNumbers, downloadUrl, firmwareSize,
         createTime, updateDescription, subType[{subName, internalName, subtype}] (the models)
The search endpoints answer "Invalid Parameter" unless the site's __CXY_* headers are sent.
Unlike the pages' embedded data (which shows only the newest 3 per model), this returns
the full version history.
"""
import argparse, collections, datetime, json, sys, time, urllib.request, uuid

API = "https://admin.crealitycloud.com"
HEADERS = {
    "Content-Type": "application/json",
    "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) nocry-catalog/0.1",
    "__CXY_OS_VER_": "not set", "__CXY_PLATFORM_": "2", "__CXY_BRAND_": "creality",
    "__CXY_APP_CH_": "not set", "__CXY_DUID_": "not set", "__CXY_APP_ID_": "cxy-gen2",
    "__CXY_APP_VER_": "7.3.28", "__CXY_OS_LANG_": "0", "__CXY_TIMEZONE_": "0",
}
# series ids as used by the site (names from its firmware pages)
SERIES = {"9": ("Flagship Series", "flagship-series"), "2": ("Ender Series", "ender-series"),
          "1": ("CR Series", "cr-series"), "5": ("HALOT Series", "halot-series"),
          "7": ("Sermoon Series", "sermoon-series"), "3": ("Other - FFF", "other-fff"),
          "69129500": ("CFS-C Series", "cfs-c-series"), "77814183": ("Hi Series", "hi-series")}
PRINTER_TYPE = 2
ACCESSORY_TYPES = range(1, 61)
# the site's own names for its firmware "types" (from its scripts)
TYPE_NAMES = {1: "Creality Box", 3: "DWIN screen", 4: "IPC camera", 5: "Raspberry Pi image", 14: "Raspberry Pi",
              16: "Laser", 17: "Piocreat Box", 18: "BLTouch", 19: "CR Touch", 20: "Creality 3D Pad",
              21: "Creality Sonic Pad", 24: "Nebula Pad", 26: "CR-Scan 3D", 27: "CFS", 30: "M1", 32: "Creality Inspect"}


def post(path, body, tries=4):
    for attempt in range(tries):
        req = urllib.request.Request(API + path, json.dumps(body).encode(),
                                     dict(HEADERS, __CXY_REQUESTID_=str(uuid.uuid4())))
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                o = json.load(r)
            if o.get("code") == 0:
                return o.get("result") or {}
            if attempt == tries - 1:
                raise RuntimeError(f"{path} {body}: {o.get('code')} {o.get('msg')}")
        except OSError as e:
            if attempt == tries - 1:
                raise
            print(f"  retry {path}: {e}", file=sys.stderr)
        time.sleep(1.5 * (attempt + 1))


def search(**params):
    out, page = [], 1
    while True:
        r = post("/api/cxy/search/firmwareSearch", dict(params, page=page, pageSize=100))
        lst = r.get("list") or []
        out += lst
        if not lst or len(out) >= (r.get("count") or 0):
            return out
        page += 1
        time.sleep(0.3)


def entry(rec):
    notes = [n.strip() for n in (rec.get("updateDescription") or []) if n and n.strip()]
    hw = [h for h in (rec.get("hardwareVersionNumbers") or []) if h] or \
         ([rec["hardwareVersionNumber"]] if rec.get("hardwareVersionNumber") else [])
    return {
        "version": (rec.get("versionNumber") or "").lstrip("Vv"),
        "file": rec.get("name") or "",
        "hardware_versions": hw,
        "released": datetime.datetime.fromtimestamp(rec.get("createTime") or 0, datetime.timezone.utc).date().isoformat(),
        "size_bytes": rec.get("firmwareSize") or 0,
        "download_url": rec.get("downloadUrl") or "",
        "notes": notes,
        "_t": rec.get("createTime") or 0,
    }


def group(records, printers_by_id):
    """records -> {model key: {model, model_id, internal_name, firmwares[]}}"""
    models = collections.OrderedDict()
    for rec in records:
        subs = rec.get("subType") or [{"subName": "(not assigned to a model)", "subtype": "", "internalName": ""}]
        names = [s.get("subName") or "" for s in subs]
        for s in subs:
            key = s.get("subtype") or s.get("subName")
            p = printers_by_id.get(s.get("subtype") or "", {})
            m = models.setdefault(key, {"model": s.get("subName") or p.get("name") or "?",
                                        "model_id": s.get("subtype") or "",
                                        "internal_name": s.get("internalName") or p.get("internalName") or "",
                                        "firmwares": []})
            e = entry(rec)
            others = [n for n in names if n != m["model"]]
            if others:
                e["also_applies_to"] = others
            # the site lists one record per hardware version: merge them
            same = next((f for f in m["firmwares"] if f["download_url"] == e["download_url"]), None)
            if same:
                same["hardware_versions"] = sorted(set(same["hardware_versions"]) | set(e["hardware_versions"]))
                same["_t"] = max(same["_t"], e["_t"])
            else:
                m["firmwares"].append(e)
    for m in models.values():
        m["firmwares"].sort(key=lambda f: f["_t"], reverse=True)
        for f in m["firmwares"]:
            del f["_t"]
        m["firmware_count"] = len(m["firmwares"])
    return sorted(models.values(), key=lambda m: m["model"].lower())


def q(s):
    return json.dumps(s, ensure_ascii=False)     # a JSON string is a valid YAML scalar


def to_yaml(cat):
    L = ["# Creality firmware catalog", f"# Source: {API}/api/cxy/search/firmwareSearch (the API behind",
         "#         https://www.crealitycloud.com/downloads/firmware)",
         f"# Built: {cat['built']} by nocry tools/catalog/build_creality_catalog.py",
         "# series -> models -> firmware versions (newest first, complete history).", "", "series:"]
    for s in cat["series"]:
        L += [f"  - name: {q(s['name'])}", f"    slug: {s['slug']}", f"    series_id: {q(s['series_id'])}", "    models:"]
        for m in s["models"]:
            L += [f"      - model: {q(m['model'])}", f"        model_id: {q(m['model_id'])}",
                  f"        internal_name: {q(m['internal_name'])}", f"        firmware_count: {m['firmware_count']}",
                  "        firmwares:"]
            for f in m["firmwares"]:
                L += [f"          - version: {q(f['version'])}", f"            file: {q(f['file'])}",
                      f"            hardware_version: {q(', '.join(f['hardware_versions']))}",
                      f"            released: {f['released']}", f"            size_bytes: {f['size_bytes']}",
                      f"            download_url: {q(f['download_url'])}"]
                if f.get("also_applies_to"):
                    L += ["            also_applies_to: [" + ", ".join(q(n) for n in f["also_applies_to"]) + "]"]
                if f["notes"]:
                    L += ["            notes:"] + [f"              - {q(n)}" for n in f["notes"]]
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-o", "--output", required=True, help="YAML file to write")
    ap.add_argument("--json", help="also write the catalog as JSON")
    a = ap.parse_args()

    printers = post("/api/cxy/v2/device/firmwarePrinterList", {"isAll": True}).get("list") or []
    by_id = {p.get("dbId"): p for p in printers}
    series_ids = sorted({str(p["seriesId"]) for p in printers}, key=lambda s: (s not in SERIES, int(s)))
    print(f"{len(printers)} printers in {len(series_ids)} series", file=sys.stderr)

    cat = {"built": datetime.date.today().isoformat(), "series": []}
    for sid in series_ids:
        recs = search(type=PRINTER_TYPE, seriesId=sid)
        name, slug = SERIES.get(sid, (f"Series {sid}", f"series-{sid}"))
        models = group(recs, by_id)
        print(f"  {name}: {len(recs)} records, {len(models)} models", file=sys.stderr)
        if models:
            cat["series"].append({"name": name, "slug": slug, "series_id": sid, "models": models})
    # accessories: one firmware "type" each (Nebula Pad = 24, Sonic Pad, boxes...)
    names = {}
    for kind in ("firmwareRecommendList", "softwareRecommendList"):
        try:
            for p in post(f"/api/cxy/v2/firmware/{kind}", {"page": 1, "pageSize": 200}).get("list") or []:
                names.setdefault(p.get("type"), p.get("name"))
        except Exception:
            pass
    acc = []
    for t in ACCESSORY_TYPES:
        if t == PRINTER_TYPE:
            continue
        try:
            recs = search(type=t)
        except Exception:
            continue
        recs = [r for r in recs if (r.get("downloadUrl") or "").strip()]
        if not recs:
            continue
        label = TYPE_NAMES.get(t) or names.get(t) or f"Type {t}"
        for r in recs:
            r["subType"] = [{"subName": label, "subtype": f"type-{t}", "internalName": ""}]
        acc += group(recs, {})
        print(f"  accessory type {t} ({label}): {len(recs)} records", file=sys.stderr)
        time.sleep(0.2)
    if acc:
        cat["series"].append({"name": "Pads, boxes and accessories", "slug": "accessories", "series_id": "", "models": acc})

    open(a.output, "w", encoding="utf-8").write(to_yaml(cat))
    if a.json:
        json.dump(cat, open(a.json, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    nm = sum(len(s["models"]) for s in cat["series"])
    nf = sum(m["firmware_count"] for s in cat["series"] for m in s["models"])
    print(f"wrote {a.output}: {len(cat['series'])} series, {nm} models, {nf} firmware entries", file=sys.stderr)


if __name__ == "__main__":
    main()
