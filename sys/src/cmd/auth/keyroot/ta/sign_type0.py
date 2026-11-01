#!/usr/bin/env python3
"""Sign a TA as a plain SHDR_TA (type 0, no bootstrap/version header).

Usage:
  sign_type0.py --key oem_privkey.pem --in ta.stripped.elf --out ta.ta

The signed image is: struct shdr (20) | sha256 | RSA-PSS signature | ELF.
The hash covers the shdr and the ELF; there is no shdr_bootstrap_ta, so
the core skips check_update_version() and needs no REE-FS version DB.
"""
import argparse
import struct

from Crypto.Hash import SHA256
from Crypto.PublicKey import RSA
from Crypto.Signature import pss

SHDR_MAGIC = 0x4F545348
SHDR_TA = 0
TEE_ALG_RSASSA_PKCS1_PSS_MGF1_SHA256 = 0x70414930


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--key", required=True)
    ap.add_argument("--in", dest="inf", required=True)
    ap.add_argument("--out", dest="outf", required=True)
    args = ap.parse_args()

    img = open(args.inf, "rb").read()
    key = RSA.import_key(open(args.key).read())
    digest_len = SHA256.digest_size
    sig_len = key.size_in_bytes()

    shdr = struct.pack("<IIIIHH", SHDR_MAGIC, SHDR_TA, len(img),
                       TEE_ALG_RSASSA_PKCS1_PSS_MGF1_SHA256,
                       digest_len, sig_len)

    h = SHA256.new()
    h.update(shdr)
    h.update(img)
    digest = h.digest()
    sig = pss.new(key).sign(h)

    with open(args.outf, "wb") as f:
        f.write(shdr)
        f.write(digest)
        f.write(sig)
        f.write(img)

    print("wrote %s: type=0 img=%d total=%d" %
          (args.outf, len(img), len(shdr) + digest_len + sig_len + len(img)))


if __name__ == "__main__":
    main()
