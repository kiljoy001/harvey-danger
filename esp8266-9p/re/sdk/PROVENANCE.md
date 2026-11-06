# NONOS SDK 2.0.0 - provenance

Fetched during B3 to name ROM targets and fingerprint-match blob
functions. Recorded before any of its symbols were trusted.

- Source: https://github.com/espressif/ESP8266_NONOS_SDK
- Tag: v2.0.0, commit 06b117d691cb2cd1a20d6d0f000b76cf65b2ad67
  (from `git ls-remote --tags`, 2026-11-02)
- Tarball: ESP8266_NONOS_SDK-06b117d.tar.gz (archive of that commit)
  SHA-256: 38e1cda4ee93e3794620f59a9c92cc4ae520e8480f08e0be8d88e2e804fb436b
- Extracted to: nonos-2.0.0/ in this directory
- Key inputs and their SHA-256:
  - lib/libnet80211.a 497650332986ee50efeb8291c4407aa129b0abbb9cf959442daccae2e4d4fc62
  - lib/libphy.a      ea176be3242869eef72b7a3b737ba161680193963d3cc5230d72c7aa4b98304d
  - lib/libpp.a       14353122e77009a7826877a4b7249ac1b6aab0b2f72588fe96a0425028bb1d36
  - lib/libmain.a     4229601cf732c83aba3266e2db20ebf01f50d08d4c69c69821efed78a558d8b8
  - ld/eagle.rom.addr.v6.ld 5cc92ba3e33bcc52135b304a2ed97df575ac9f9fa13a88fb4f47a8a815c23fe4

## Trust caveat (important)

The board's image reports SDK 2.0.0 but was compiled 2016-08-09
(string at flash 0x059a71), and its AT layer is dated 2016-07-14.
The v2.0.0 tag is the closest public release, not necessarily the
exact build. Fingerprint results agree: 32 of 101 sampled Wi-Fi
functions match strongly (>=3 code windows), 26 weakly, 43 not at
all. SDK names are therefore used only where byte fingerprints,
exact size, or anchor+behaviour evidence corroborate them; a name
from this SDK alone is a lead, not a fact. Local (static) functions
are stripped in these libraries too, so most helper names remain
unrecoverable from the SDK.

The SDK is Espressif's, under its own licence in nonos-2.0.0/License.
It is an RE reference only; nothing from it goes into the plan9ized
firmware.

