"""Exercise publication gates using disposable Git repositories and fake data.

All credential fixtures are assembled in memory. No network, hardware, or
installed runtime state is accessed. A local bare Git repository models upload.
"""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("privacy_guard_under_test", ROOT / "scripts/privacy_guard.py")
GUARD = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GUARD
SPEC.loader.exec_module(GUARD)
POLICY = json.loads((ROOT / ".privacy-policy.json").read_bytes())
ZERO_OID = "0" * 40


def generated_value() -> str:
    return "synthetic-" + secrets.token_hex(16)


def assignment(field: str, value: str) -> str:
    return field + " = " + json.dumps(value) + "\n"


def binary_path() -> Path:
    configured = os.environ.get("GITLEAKS_BIN")
    if configured:
        return Path(configured).resolve()
    common = subprocess.run(
        ["git", "rev-parse", "--git-common-dir"], cwd=ROOT,
        capture_output=True, check=True,
    ).stdout.decode().strip()
    return (ROOT / common / "privacy-tools/gitleaks").resolve()


class BlobRulesTests(unittest.TestCase):
    def rules(self, path, text, mode="100644", policy=POLICY):
        data = text.encode() if isinstance(text, str) else text
        return {finding.rule for finding in GUARD.scan_blob(path, data, mode, policy)}

    def test_wifi_credentials_in_cpp_json_ini_and_platformio(self):
        sample = generated_value()
        field = "wifi_" + "password"
        cases = [
            ("src/settings.cpp", "const char *" + field + " = " + json.dumps(sample) + ";\n"),
            ("settings.json", json.dumps({field: sample})),
            ("settings.ini", field + "=" + sample + "\n"),
            ("platformio.ini", "build_flags = -D" + field.upper() + "=" + sample + "\n"),
            ("src/network.cpp", "Wi" + "Fi.begin(" + json.dumps(sample) + ", " + json.dumps(sample) + ");"),
            ("src/store.cpp", "prefs.putString(" + json.dumps("pass") + ", " + json.dumps(sample) + ");"),
        ]
        for path, text in cases:
            with self.subTest(path=path):
                self.assertIn("credential-literal", self.rules(path, text))

    def test_pairing_literals_and_environment_placeholders(self):
        field = "pairing_" + "key"
        self.assertIn("credential-literal", self.rules("settings.json", json.dumps({field: generated_value()})))
        self.assertEqual(set(), self.rules(".env.example", field + "=${PAIRING_KEY}\n"))

    def test_test_source_contains_no_publishable_sensitive_fixtures(self):
        path = Path(__file__)
        self.assertEqual(set(), self.rules("tests/privacy/" + path.name, path.read_bytes()))

    def test_exact_fixture_exemption_does_not_cover_mutation_rule_or_path(self):
        path = "mac_bridge/tests/test_codex.py"
        field = "pass" + "word"
        original = assignment(field, "x")
        self.assertEqual(set(), self.rules(path, original))
        self.assertIn("credential-literal", self.rules(path, assignment(field, "x" + generated_value())))
        self.assertIn("credential-literal", self.rules("tests/test_unreviewed.py", original))
        self.assertFalse(GUARD.exempt(POLICY, path, "key-shape", "x"))

    def test_private_files_and_renamed_runtime_records(self):
        for path in [".runtime/status.json", "device.log", "events.jsonl", ".env", "backup.bin"]:
            with self.subTest(path=path):
                self.assertIn("private-file", self.rules(path, "ordinary text\n"))
        record = {"session_id": generated_value(), "timestamp": 1, "messages": []}
        self.assertIn("runtime-record", self.rules("notes.txt", json.dumps(record)))
        self.assertIn("runtime-record", self.rules("capture.txt", "TOUCH_METRICS samples=" + "17"))

    def test_pairing_cli_material_and_encoded_literal_are_blocked(self):
        sample = secrets.token_hex(16)
        field = "pairing_" + "key"
        encoded = field + " = bytes.fromhex(" + json.dumps(sample) + ")\n"
        self.assertTrue(self.rules("config.py", encoded))
        self.assertTrue(self.rules("pairing.txt", "ordinary copied material\n"))
        for size in [16, 32]:
            with self.subTest(hex_bytes=size):
                self.assertTrue(self.rules("notes.txt", secrets.token_hex(size) + "\n"))
        self.assertTrue(self.rules("notes.txt", "Bridge pairing code\n" + sample + "\n"))

    def test_neutral_json_field_does_not_hide_pairing_sized_material(self):
        document = json.dumps({"value": secrets.token_hex(16)})
        self.assertIn("opaque-secret-material", self.rules("settings.json", document))

    def test_shell_unquoted_wifi_export_and_pairing_assignment_are_blocked(self):
        sample = generated_value()
        statements = ["export " + ("wifi_" + "password").upper() + "=" + sample,
                      "pairing" + "key=" + sample]
        for statement in statements:
            with self.subTest(form=statement.split("=", 1)[0]):
                self.assertIn("credential-literal", self.rules("setup.sh", statement + "\n"))

    def test_renamed_project_logs_and_unordered_session_json_are_blocked(self):
        lines = ["HEART" + "BEAT uptime_ms=21", "AUTH" + "_OK claude_companion=true",
                 "HUB_ACTION" + "_COMPLETED action=refresh"]
        for line in lines:
            with self.subTest(record=line.split()[0]):
                self.assertIn("runtime-record", self.rules("diagnostics.txt", line + "\n"))
        record = {"messages": [], "timestamp": 1, "session_id": generated_value()}
        self.assertIn("runtime-record", self.rules("notes.txt", json.dumps(record)))

    def test_renamed_multiline_json_records_are_checked_individually(self):
        records = [{"messages": [], "session_id": generated_value()},
                   {"timestamp": 1, "session_id": generated_value()}]
        data = "\n".join(json.dumps(record) for record in records).encode()
        findings = GUARD.scan_blob("capture.txt", data, "100644", POLICY)
        self.assertEqual(2, sum(finding.rule == "runtime-record" for finding in findings))

    def test_personal_network_identity_and_paths(self):
        samples = [
            ("personal-path", "/" + "Users/Example/person.txt"),
            ("private-network", ".".join(["192", "168", "8", "9"])),
            ("device-mac", ":".join(["ab", "cd", "ef", "12", "34", "56"])),
            ("personal-email", "example-person" + "@" + "example.invalid"),
            ("personal-phone", "138" + "12345678"),
        ]
        for rule, sample in samples:
            with self.subTest(rule=rule):
                self.assertIn(rule, self.rules("notes.txt", sample))

    def test_symlink_and_gitlink_blocked(self):
        for mode in ["120000", "160000"]:
            with self.subTest(mode=mode):
                self.assertIn("unsupported-git-entry", self.rules("link", b"target", mode))

    def test_unknown_binary_and_oversize_blocked(self):
        self.assertIn("unreviewed-binary", self.rules("image.dat", b"\xff\x00"))
        self.assertIn("oversized-unreviewed-file", self.rules("large.txt", b"a" * (GUARD.MAX_TEXT_BYTES + 1)))

    def test_pinned_fonts_require_exact_bytes(self):
        for path in POLICY["binary_assets"]:
            with self.subTest(path=path):
                data = (ROOT / path).read_bytes()
                self.assertEqual(set(), self.rules(path, data))
                self.assertIn("changed-binary-asset", self.rules(path, data + b"changed"))


class HookIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scanner = binary_path()
        if not cls.scanner.is_file():
            raise RuntimeError("Pinned Gitleaks is required to run privacy integration tests.")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="privacy-guard-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.repo = self.base / "working"
        self.repo.mkdir()
        self.hidden_values = []
        self.environment = dict(os.environ, GITLEAKS_BIN=str(self.scanner))
        for name in ["GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT"]:
            self.environment.pop(name, None)
        self.run_git("init", "-b", "main")
        self.configure(self.repo)
        for name in ["scripts/privacy_guard.py", ".privacy-policy.json", ".githooks/pre-commit", ".githooks/pre-push"]:
            target = self.repo / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, target)
        self.write("README.md", "Disposable privacy guard fixture.\n")
        self.run_git("add", ".")
        self.commit("baseline", bypass=True)
        self.run_git("config", "core.hooksPath", ".githooks")
        self.remote = self.base / "remote.git"
        self.run_command(["git", "init", "--bare", str(self.remote)])
        self.run_git("remote", "add", "origin", str(self.remote))

    def run_command(self, command, cwd=None, input_text=None, env=None):
        result = subprocess.run(
            command, cwd=cwd or self.repo, input=input_text, text=True,
            capture_output=True, env=env or self.environment, timeout=60,
        )
        output = result.stdout + result.stderr
        for sample in self.hidden_values:
            self.assertFalse(sample in output, "Command output exposed a synthetic credential.")
        return result

    def run_git(self, *args, cwd=None, expected=0):
        result = self.run_command(["git", *args], cwd=cwd)
        if expected is not None:
            self.assertEqual(expected, result.returncode, "Unexpected Git exit code; output suppressed for privacy.")
        return result

    def configure(self, repo):
        self.run_git("config", "user.name", "Privacy Guard Test", cwd=repo)
        self.run_git("config", "user.email", "privacy-test@users.noreply.github.com", cwd=repo)
        self.run_git("config", "commit.gpgsign", "false", cwd=repo)

    def write(self, name, data):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode() if isinstance(data, str) else data)
        return path

    def sensitive(self):
        sample = generated_value()
        self.hidden_values.append(sample)
        return sample

    def webhook(self):
        sample = "https://" + "hooks.slack.com/" + "services/T" + "12345678/B" + "12345678/" + secrets.token_hex(16)
        self.hidden_values.append(sample)
        return sample

    def commit(self, message, bypass=False, expected=0):
        options = ["-c", "core.hooksPath=/dev/null"] if bypass else []
        return self.run_git(*options, "commit", "-m", message, expected=expected)

    def guard(self, mode, input_text=None, env=None, cwd=None):
        return self.run_command([sys.executable, "scripts/privacy_guard.py", mode], cwd=cwd, input_text=input_text, env=env)

    def refs(self):
        return self.run_git("--git-dir=" + str(self.remote), "for-each-ref", "--format=%(refname) %(objectname)").stdout

    def assert_blocked(self, result, rule=None):
        self.assertNotEqual(0, result.returncode, "Private input unexpectedly passed.")
        self.assertIn("Privacy guard BLOCKED", result.stderr)
        if rule:
            self.assertIn(rule, result.stderr)

    def initial_push(self):
        result = self.run_git("push", "origin", "main")
        self.assertIn("Privacy guard PASS", result.stdout)

    def test_normal_pre_commit_rejects_secret_before_commit(self):
        before = self.run_git("rev-parse", "HEAD").stdout
        self.write("settings.ini", assignment("wifi_" + "password", self.sensitive()))
        self.run_git("add", "settings.ini")
        result = self.commit("must be blocked", expected=None)
        self.assert_blocked(result, "credential-literal")
        self.assertEqual(before, self.run_git("rev-parse", "HEAD").stdout)

    def test_staged_snapshot_is_authoritative(self):
        self.write("notes.txt", "clean staged content\n")
        self.run_git("add", "notes.txt")
        self.write("notes.txt", assignment("pairing_" + "key", self.sensitive()))
        self.assertEqual(0, self.guard("--staged").returncode)
        self.run_git("add", "notes.txt")
        self.write("notes.txt", "clean working copy\n")
        self.assert_blocked(self.guard("--staged"), "credential-literal")

    def test_clean_commit_and_normal_push_succeed(self):
        self.write("notes.txt", "Ordinary source changes.\n")
        self.run_git("add", "notes.txt")
        self.commit("clean update")
        self.initial_push()
        self.assertEqual(self.run_git("rev-parse", "HEAD").stdout.strip(), self.run_git("--git-dir=" + str(self.remote), "rev-parse", "refs/heads/main").stdout.strip())

    def test_pre_push_rejects_added_then_deleted_secret_in_history(self):
        self.initial_push()
        before = self.refs()
        path = self.write("settings.ini", assignment("pairing_" + "key", self.sensitive()))
        self.run_git("add", "settings.ini")
        self.commit("private intermediate snapshot", bypass=True)
        path.unlink()
        self.run_git("add", "-u")
        self.commit("remove private snapshot", bypass=True)
        self.assertFalse(path.exists())
        result = self.run_git("push", "origin", "main", expected=None)
        self.assert_blocked(result, "credential-literal")
        self.assertEqual(before, self.refs(), "Blocked push changed the remote refs.")

    def test_multi_ref_push_blocks_all_updates_when_one_is_private(self):
        self.initial_push()
        before = self.refs()
        self.run_git("checkout", "-b", "private-branch")
        self.write("separate.ini", assignment("wifi_" + "password", self.sensitive()))
        self.run_git("add", "separate.ini")
        self.commit("private branch fixture", bypass=True)
        self.run_git("checkout", "main")
        self.write("notes.txt", "clean main update\n")
        self.run_git("add", "notes.txt")
        self.commit("clean branch fixture")
        result = self.run_git("push", "origin", "main", "private-branch", expected=None)
        self.assert_blocked(result, "credential-literal")
        self.assertEqual(before, self.refs(), "Multi-ref rejection changed the remote refs.")

    def test_deletion_only_push_succeeds_even_without_scanner(self):
        self.initial_push()
        self.run_git("push", "origin", "main:refs/heads/remove-me")
        environment = dict(self.environment, GITLEAKS_BIN=str(self.base / "absent-tool"))
        result = self.run_command(["git", "push", "origin", ":refs/heads/remove-me"], env=environment)
        self.assertEqual(0, result.returncode, "Deletion-only push should expose no objects.")
        self.assertIn("ref deletions only", result.stdout)
        self.assertNotIn("refs/heads/remove-me", self.refs())

    def test_gitleaks_detects_api_token_and_redacts_output(self):
        prefix = "gh" + "p_"
        sample = prefix + secrets.token_hex(20)
        self.hidden_values.append(sample)
        self.write("client.txt", "authorization_value = " + json.dumps(sample) + "\n")
        self.run_git("add", "client.txt")
        result = self.guard("--staged")
        self.assert_blocked(result, "gitleaks:")

    def test_attributes_cannot_hide_secret_from_history_scanning(self):
        self.initial_push()
        before = self.refs()
        self.write(".gitattributes", "*.txt -diff\n")
        path = self.write("hidden.txt", self.webhook() + "\n")
        self.run_git("add", ".gitattributes", "hidden.txt")
        self.commit("attribute-hidden secret fixture", bypass=True)
        path.unlink()
        self.run_git("add", "-u")
        self.commit("remove hidden secret fixture", bypass=True)
        result = self.run_git("push", "origin", "main", expected=None)
        self.assert_blocked(result, "gitleaks:")
        self.assertEqual(before, self.refs(), "Attribute-hidden secret reached the remote.")

    def test_clean_tree_commit_message_secret_blocks_push(self):
        self.initial_push()
        before = self.refs()
        sample = self.webhook()
        self.run_git("-c", "core.hooksPath=/dev/null", "commit", "--quiet", "--allow-empty", "-m", sample)
        result = self.run_git("push", "origin", "main", expected=None)
        self.assert_blocked(result, "gitleaks:")
        self.assertEqual(before, self.refs(), "Private commit metadata reached the remote.")

    def test_annotated_tag_message_secret_blocks_tag_push(self):
        self.initial_push()
        before = self.refs()
        self.run_git("tag", "-a", "private-tag", "-m", self.webhook())
        result = self.run_git("push", "origin", "refs/tags/private-tag", expected=None)
        self.assert_blocked(result, "gitleaks:")
        self.assertEqual(before, self.refs(), "Private tag metadata reached the remote.")

    def test_personal_commit_identity_blocks_push(self):
        self.initial_push()
        before = self.refs()
        sample = "fixture-" + secrets.token_hex(6) + "@" + "example.invalid"
        self.hidden_values.append(sample)
        environment = dict(self.environment, GIT_AUTHOR_EMAIL=sample)
        result = self.run_command(["git", "-c", "core.hooksPath=/dev/null", "commit", "--quiet", "--allow-empty", "-m", "private identity fixture"], env=environment)
        self.assertEqual(0, result.returncode, "Unable to construct private identity fixture.")
        result = self.run_git("push", "origin", "main", expected=None)
        self.assert_blocked(result, "personal-email")
        self.assertEqual(before, self.refs(), "Private author metadata reached the remote.")

    def test_missing_scanner_is_fail_closed(self):
        environment = dict(self.environment, GITLEAKS_BIN=str(self.base / "absent-tool"))
        self.assert_blocked(self.guard("--staged", env=environment), "cannot run")

    def test_wrong_scanner_version_and_unsafe_scanner_output_are_blocked(self):
        fake = self.base / "fake-scanner"
        fake.write_text("#!" + sys.executable + "\nprint('0.0.0')\n")
        fake.chmod(0o700)
        environment = dict(self.environment, GITLEAKS_BIN=str(fake))
        self.assert_blocked(self.guard("--staged", env=environment), "Pinned Gitleaks")
        sample = self.sensitive()
        fake.write_text(
            "#!" + sys.executable + "\nimport sys\n"
            "if sys.argv[1:] == ['version']:\n    print(" + repr(GUARD.GITLEAKS_VERSION) + ")\n"
            "else:\n    print(" + repr(sample) + ")\n    print(" + repr(sample) + ", file=sys.stderr)\n"
            "    raise SystemExit(2)\n"
        )
        self.assert_blocked(self.guard("--staged", env=environment), "no external output displayed")

    def test_malformed_pre_push_input_is_fail_closed(self):
        self.assert_blocked(self.guard("--pre-push", input_text="unexpected input\n"), "Malformed")

    def test_shallow_history_is_fail_closed(self):
        shallow = self.base / "shallow"
        self.run_git("clone", "--depth=1", self.repo.as_uri(), str(shallow))
        self.assert_blocked(self.guard("--history", cwd=shallow), "Shallow history")

    def test_staged_symlink_unknown_binary_and_renamed_logs_are_blocked(self):
        cases = [("opaque.dat", b"\xff\x00", "unreviewed-binary"),
                 ("capture.txt", "WIFI_NETWORK ip=" + ".".join(["192", "168", "8", "9"]), "runtime-record")]
        for path, data, rule in cases:
            with self.subTest(rule=rule):
                self.write(path, data)
                self.run_git("add", path)
                self.assert_blocked(self.guard("--staged"), rule)
                self.run_git("rm", "--cached", path)
                (self.repo / path).unlink()
        os.symlink("README.md", self.repo / "linked-source")
        self.run_git("add", "linked-source")
        self.assert_blocked(self.guard("--staged"), "unsupported-git-entry")


if __name__ == "__main__":
    unittest.main()
