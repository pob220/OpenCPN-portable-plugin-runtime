#!/usr/bin/env python3
"""Generate disposable Ed25519 keys for local portable-package tests."""

from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def main() -> None:
    output_dir = Path(__file__).resolve().parent
    private_path = output_dir / "igrib-ed25519-private.pem"
    public_path = output_dir / "igrib-ed25519-public.pem"
    if private_path.exists() or public_path.exists():
        raise SystemExit(
            "Refusing to overwrite an existing development key; delete both "
            "generated PEM files first"
        )

    private_key = Ed25519PrivateKey.generate()
    private_path.write_bytes(
        private_key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )
    private_path.chmod(0o600)
    public_path.write_bytes(
        private_key.public_key().public_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PublicFormat.SubjectPublicKeyInfo,
        )
    )
    print(f"generated disposable test key: {public_path}")


if __name__ == "__main__":
    main()
