# Creality firmware catalog

`build_creality_catalog.py -o firmwares.yaml [--json firmwares.json]` lists every firmware Creality publishes, with full version history, from the API behind <https://www.crealitycloud.com/downloads/firmware>. No login is needed. The method (endpoints, parameters, required headers) is described at the top of the script.

Findings (2026-11-26):
- 116 printers in 10 series, plus accessories (Nebula Pad, Sonic Pad, Creality Box...): 120 models, 634 entries, 551 distinct files.
- The hex string in a download URL (`.../file/<32 hex>/<name>`) is the file's md5, so a download can be verified without any other source. Checked on `NEBULA_ota_img_V1.1.0.30.img`.
- Ingenic OTA files (`*_ota_img_V*.img`, 7z) are read by `nocry` directly; see `host/README.md`.

nocry's built-in list (`host/core/data/catalog.json`) also holds NebulaOS, which does not come from Creality: `nocry catalog --refresh` asks GitHub's releases API for it and writes the combined list, which is then copied into the source tree. See `docs/nebulaos.md`.
