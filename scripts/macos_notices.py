"""Copy and check the license texts named by the bundled Qt SPDX JSON files.

This conservative coverage rule also includes optional components and license
alternatives in the module metadata; it does not assert that all are compiled.
"""
import json
from pathlib import Path
import re
import shutil

QT_NOTICES = ("qtbase", "qtsvg", "qtmultimedia")
LICENSE_FIELDS = ("licenseConcluded", "licenseDeclared", "licenseInfoFromFiles",
                  "licenseInfoInFiles", "licenseInfoInSnippets")
KEYWORDS = {"AND", "OR", "WITH", "NONE", "NOASSERTION"}
IDENTIFIER = re.compile(r"(?:DocumentRef-[A-Za-z0-9.-]+:)?[A-Za-z0-9][A-Za-z0-9.+-]*")
ROOT = Path(__file__).resolve().parents[1]
FFMPEG_LIBRARIES = {"libavcodec.61.dylib", "libavformat.61.dylib", "libavutil.59.dylib",
                    "libswresample.5.dylib", "libswscale.8.dylib"}


def ffmpeg_provenance(root=ROOT):
    """The reviewed dependency tuple, not a version inferred from stale SPDX."""
    info = json.loads((root / "packaging/macos/ffmpeg.json").read_text())
    qt = json.loads((root / "packaging/macos/qt.json").read_text())
    if (info["qt_version"] != qt["qt_version"]
            or info["qt_multimedia_archive_sha256"] not in {item["sha256"] for item in qt["archives"]
                                                          if "qtmultimedia-" in item["path"]}
            or set(info["arm64_configuration_offsets"]) != FFMPEG_LIBRARIES):
        raise ValueError("Review the FFmpeg source tuple and byte ranges for this Qt dependency")
    return info


def ffmpeg_source_note(info):
    return f"""
## FFmpeg in this macOS bundle

The official Qt {info['qt_version']} macOS libraries contain FFmpeg {info['ffmpeg_version']}.
The unchanged module SPDX lists {info['upstream_spdx_ffmpeg_version']} from Qt's static
attribution entry; its actual build comment instead names FFmpeg-n{info['ffmpeg_version']}.
`FFMPEG-SOURCE.json` supplies the platform-specific source/configuration reference:

- {info['source_url']}
- Source SHA-256: `{info['source_sha256']}`
- Qt's source SHA-1 pin: `{info['source_sha1']}`
- FFmpeg tag `n{info['ffmpeg_version']}`, commit `{info['ffmpeg_commit']}`.

Official Qt provisioning, commit `{info['qt5_commit']}`:

- https://github.com/qt/qt5/blob/{info['qt5_commit']}/coin/provisioning/common/unix/ffmpeg-installation-utils.sh
- https://github.com/qt/qt5/blob/{info['qt5_commit']}/coin/provisioning/common/macos/install-ffmpeg-macos.sh
- https://github.com/qt/qt5/blob/{info['qt5_commit']}/coin/provisioning/common/shared/ffmpeg_config_options.txt

The reviewed path downloads the pinned source, configures shared libraries,
adjusts install-name references and joins the architectures. It contains no
FFmpeg source-patch step. Zerus retains the arm64 slices and checks their fixed
reviewed version/configuration byte ranges before signing. The source archive
includes `COPYING.LGPLv2.1`, `LICENSE.md` and the component sources.
These references do not claim an independently reproduced binary or historical
Qt vendor build attestation; the original SPDX is preserved unchanged.
"""


def ffmpeg_findings(bundle, info):
    """Check only reviewed fixed arm64 spans; never load or execute a library.

    The pinned Qt archive and existing arm64 bundle check establish which
    dependency these offsets address. This is not whole-library integrity or a
    general Mach-O decoder. A new dependency requires new reviewed ranges.
    """
    probes = {name: [(offset, info["configuration"].encode() + b"\0")]
              for name, offset in info["arm64_configuration_offsets"].items()}
    version = info["arm64_version_info"]
    probes[version["library"]] += [(version["code_offset"], bytes.fromhex(version["code_hex"])),
                                   (version["value_offset"], info["ffmpeg_version"].encode() + b"\0")]
    findings = []
    for name, spans in sorted(probes.items()):
        try:
            with (bundle / "Contents/Frameworks" / name).open("rb") as stream:
                for offset, expected in spans:
                    stream.seek(offset)
                    if stream.read(len(expected)) != expected:
                        findings.append(f"{name}: reviewed FFmpeg version/configuration differs at {offset}")
        except OSError as error:
            findings.append(f"{name}: cannot check FFmpeg source provenance: {error}")
    return findings


def document_licenses(document):
    """Return the named identifiers and nonempty local LicenseRef definitions."""
    if not isinstance(document, dict) or not isinstance(document.get("packages"), list):
        raise ValueError("SPDX packages must be an array")
    named = set()
    for collection in ("packages", "files", "snippets"):
        entries = document.get(collection, [])
        if not isinstance(entries, list) or any(not isinstance(entry, dict) for entry in entries):
            raise ValueError(f"SPDX {collection} must contain objects")
        for entry in entries:
            for field in LICENSE_FIELDS:
                values = entry.get(field, [])
                values = [values] if isinstance(values, str) else values
                if not isinstance(values, list) or any(not isinstance(value, str) for value in values):
                    raise ValueError(f"SPDX {field} must contain strings")
                for value in values:
                    if IDENTIFIER.sub("", value).strip(" \t\r\n()"):
                        raise ValueError(f"Invalid SPDX expression: {value!r}")
                    named.update(token for token in IDENTIFIER.findall(value) if token not in KEYWORDS)
    definitions = document.get("hasExtractedLicensingInfos", [])
    if not isinstance(definitions, list) or any(not isinstance(entry, dict) for entry in definitions):
        raise ValueError("SPDX extracted license definitions must be objects")
    defined = {entry.get("licenseId") for entry in definitions
               if isinstance(entry.get("licenseId"), str) and isinstance(entry.get("extractedText"), str)
               and entry["extractedText"].strip()}
    return named, defined


def missing_license_texts(documents, texts):
    """Require a full standard text or the referenced document's extracted text."""
    parsed = {name: document_licenses(document) for name, document in documents.items()}
    namespaces = {document.get("documentNamespace"): name for name, document in documents.items()
                  if isinstance(document.get("documentNamespace"), str)}
    missing = []
    for name, (named, defined) in sorted(parsed.items()):
        external = {entry.get("externalDocumentId"): namespaces.get(entry.get("spdxDocument"))
                    for entry in documents[name].get("externalDocumentRefs", []) if isinstance(entry, dict)}
        for identifier in sorted(named):
            prefix, separator, reference = identifier.rpartition(":")
            if separator:
                target = external.get(prefix)
                known = target is not None and reference in parsed[target][1]
            elif identifier.startswith("LicenseRef-"):
                known = identifier in defined
            else:
                known = identifier in texts
            if not known:
                missing.append(f"{name}: {identifier}")
    return missing


def notice_findings(directory):
    documents, findings = {}, []
    for module in QT_NOTICES:
        name = f"{module}.spdx.json"
        try:
            documents[name] = json.loads((directory / name).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            findings.append(f"{name}: could not read SPDX JSON: {error}")
    texts = {path.stem for path in directory.glob("*.txt") if path.is_file() and path.stat().st_size}
    try:
        findings += [f"missing Qt license text: {item}" for item in missing_license_texts(documents, texts)]
    except (TypeError, ValueError) as error:
        findings.append(f"invalid Qt license metadata: {error}")
    return findings


def copy_notices(root, prefix, version, destination):
    destination.mkdir(parents=True, exist_ok=True)
    # Reuse the existing canonical SPDX texts and their recorded primary-source
    # provenance. The Windows location predates their use by both packagers.
    sources = list((root / "packaging/windows/licenses").glob("*.txt"))
    sources += [root / "packaging/macos/licenses" / name for name in ("LGPL-3.0.txt", "GPL-3.0.txt")]
    for source in sources:
        shutil.copyfile(source, destination / source.name)
        (destination / source.name).chmod(0o644)
    for module in QT_NOTICES:
        target = destination / f"{module}.spdx.json"
        shutil.copyfile(prefix / "sbom" / f"{module}-{version}.spdx.json", target)
        target.chmod(0o644)
    findings = notice_findings(destination)
    if findings:
        raise ValueError("\n".join(findings))
