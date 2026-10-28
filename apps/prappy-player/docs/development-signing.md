# Local Windows development signing

This machine's Smart App Control intermittently rejected unsigned local builds.
A self-signed MSIX installed successfully but its unsigned EXE was still blocked.
The same executable, Authenticode-signed with the locally trusted development
certificate, launched and passed the complete state suite with Smart App Control
still enforced. This is measured behavior on this workstation, not a guarantee
for every Windows policy or for distribution to other people.

## Build integration

`scripts/build.ps1` signs staged EXEs and DLLs after linking and copying runtime
dependencies when the following user-local configuration exists:

```json
{"certificate_thumbprint":"YOUR_40_CHARACTER_CERTIFICATE_THUMBPRINT"}
```

Configuration path: `%USERPROFILE%/.config/prappy/development-signing.json`.
The certificate and its private key remain in the Windows certificate store.
No certificate, key, password or machine-specific signing configuration belongs
in the repository. The build uses the exact configured certificate; it doesn't
choose arbitrary signing identities or generate replacement certificates.

`sign-development.ps1` preserves already-valid vendor signatures, signs unsigned
staged binaries with SHA-256, verifies Authenticode policy, and records final
hashes/signers in `development-signatures.json`. A failed signing operation is a
failed build. Vendored dependency files remain untouched. All build variants use
the same step, including ASan builds.

This development signature is local and isn't timestamped for public delivery.
The certificate must remain valid. The build warns fourteen days before expiry
and rejects an expired/missing certificate. Renewal requires explicit local
trust of the replacement certificate; changing the private key never silently
changes Windows trust. Public distribution is a separate signing decision.

## Workstation verification, 2026-10-28

- Unsigned image SHA-256:
  `489779738589228D68E5D2E7D312D074AD686FC54237B96558A6F8E6273DF7C8`.
- Package-only signing: installed with Developer signature and Status Ok;
  launch blocked by VerifiedAndReputableDesktop, status `0xc0e90002`.
- Direct development signature: Authenticode Valid; process launched; state
  regression suite ALL PASS, including shared ImPlot waveform and PCM meter.
- Smart App Control registry state remained `1` (enforced).
- Evidence: `build/msix-trust-probe/signed-state.log` and Windows Code Integrity
  events. The development test certificate was explicitly trusted by the user.

The package probe is not part of the product and isn't required for normal
development launches after EXE signing.
