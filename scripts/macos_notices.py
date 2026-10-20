"""Copy and check the license texts named by the bundled Qt SPDX JSON files.

This conservative coverage rule also includes optional components and license
alternatives in the module metadata; it does not assert that all are compiled.
"""
import json
import re
import shutil

QT_NOTICES = ("qtbase", "qtsvg", "qtmultimedia")
LICENSE_FIELDS = ("licenseConcluded", "licenseDeclared", "licenseInfoFromFiles",
                  "licenseInfoInFiles", "licenseInfoInSnippets")
KEYWORDS = {"AND", "OR", "WITH", "NONE", "NOASSERTION"}
IDENTIFIER = re.compile(r"(?:DocumentRef-[A-Za-z0-9.-]+:)?[A-Za-z0-9][A-Za-z0-9.+-]*")


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
