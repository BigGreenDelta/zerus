"""Keep zerus-relay buildable from the files its image and packager copy.

The deployment context contains relay/ and the reviewed sibling crypto/ crate.
Only manifests, the relay lock/config and source files enter the build. Keep
every runtime dependency within that context, including transitive path crates.
"""
import re
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RELAY = ROOT / "services/mobile/relay"
SOURCES = RELAY / "src"
CRYPTO = RELAY.parent / "crypto"


class RelayInputsTest(unittest.TestCase):
    def test_the_image_copies_only_the_packaged_inputs(self):
        copies = re.findall(r"(?m)^COPY (?!--from=)(.*)$", (RELAY.parent / "Dockerfile").read_text())
        self.assertEqual(copies, ["relay/Cargo.toml relay/Cargo.lock ./", "relay/.cargo ./.cargo", "relay/src ./src",
                                  "crypto/Cargo.toml ../crypto/Cargo.toml", "crypto/src ../crypto/src"])
        self.assertIn("WORKDIR /build/relay", (RELAY.parent / "Dockerfile").read_text())

    def test_the_manifest_needs_no_other_file(self):
        manifest = tomllib.loads((RELAY / "Cargo.toml").read_text())
        self.assertFalse((RELAY / "build.rs").exists())
        self.assertNotIn("build", manifest["package"])
        for kind in ("test", "bench", "example"):
            self.assertNotIn(kind, manifest, "integration tests are discovered from tests/, never declared")
        targets = [manifest.get("lib", {}), *manifest.get("bin", [])]
        for target in targets:
            self.assertTrue(target.get("path", "src/").startswith("src/"), target)
        # Platform-specific tables, [patch] and [replace] can name a path too.
        kinds = ("dependencies", "dev-dependencies", "build-dependencies")
        tables = [manifest.get(kind, {}) for kind in kinds]
        tables += [platform.get(kind, {}) for platform in manifest.get("target", {}).values() for kind in kinds]
        tables += [*manifest.get("patch", {}).values(), manifest.get("replace", {})]
        for table in tables:
            for name, spec in table.items():
                if name == "zerus-mobile-crypto":
                    self.assertEqual(spec.get("path"), "../crypto")
                else:
                    self.assertNotIn("path", spec if isinstance(spec, dict) else {}, name)
        crypto = tomllib.loads((CRYPTO / "Cargo.toml").read_text())
        self.assertFalse((CRYPTO / "build.rs").exists())
        for table in (crypto.get(kind, {}) for kind in kinds):
            for name, spec in table.items():
                self.assertNotIn("path", spec if isinstance(spec, dict) else {}, name)
        for name in ("config", "config.toml"):
            if (RELAY / ".cargo" / name).exists():
                settings = tomllib.loads((RELAY / ".cargo" / name).read_text())
                self.assertFalse({"paths", "patch"} & set(settings), name)

    def test_sources_include_only_packaged_files(self):
        for path in [*SOURCES.rglob("*"), *(CRYPTO / "src").rglob("*")]:
            root = RELAY if path.is_relative_to(RELAY) else CRYPTO
            relative = path.relative_to(root)
            self.assertNotIn("tests", relative.parts)
            if not path.is_file():
                continue
            self.assertIn(path.suffix, {".rs", ".sql"}, relative)
            if path.suffix != ".rs":
                continue
            text = path.read_text(encoding="utf-8")
            calls = re.findall(r"\binclude(?:_str|_bytes)?!\(\s*([^)]*)\)", text)
            attributes = re.findall(r"#\[path\s*=\s*([^\]]*)\]", text)
            for argument in calls + attributes:
                with self.subTest(file=str(relative), argument=argument):
                    literal = re.fullmatch(r'"([^"\\]+)"', argument.strip())
                    self.assertIsNotNone(literal, "name the file with a plain literal")
                    included = (path.parent / literal[1]).resolve()
                    self.assertTrue(included.is_relative_to((root / "src").resolve()), included)
                    self.assertIn(included.suffix, {".rs", ".sql"})
                    self.assertTrue(included.is_file())


if __name__ == "__main__":
    unittest.main()
