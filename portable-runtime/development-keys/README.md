# Development package keys

Private signing keys are never stored in this repository. Run
`generate-development-keys.py` to create a disposable Ed25519 pair for local
package and conformance testing. The generated PEM files are ignored by Git
and may be deleted at any time.

These keys provide test coverage only and convey no production trust.
Production catalogue keys must be supplied by credentialled release
infrastructure and must never be written into the source tree.
