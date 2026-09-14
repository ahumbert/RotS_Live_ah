#!/usr/bin/env python3

import contextlib
import datetime
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parent / "deploy.py"
SPEC = importlib.util.spec_from_file_location("deploy", MODULE_PATH)
deploy = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules["deploy"] = deploy
SPEC.loader.exec_module(deploy)

TEST_ENV = deploy.ENVS["zzz-forge-test"]
SERVER = deploy.Server("someone", "example.org", 2222)


# ---------------------------------------------------------------------------------------------
# Environments and arguments
# ---------------------------------------------------------------------------------------------


class EnvTableTest(unittest.TestCase):
    def test_every_env_dir_passes_the_remote_path_guard(self) -> None:
        for env in deploy.ENVS.values():
            self.assertEqual(deploy.port_dir(env), f"/rots/{env.dir_name}")

    def test_only_the_4k_envs_turn_big_brother_off(self) -> None:
        with_edits = sorted(name for name, env in deploy.ENVS.items() if env.source_edits)

        self.assertEqual(with_edits, ["4k", "zzz-forge-test-4k"])
        self.assertEqual(deploy.ENVS["4k"].source_edits, (deploy.BIG_BROTHER_OFF,))
        self.assertEqual(deploy.BIG_BROTHER_OFF.old_line, "#define USE_BIG_BROTHER 1")
        self.assertEqual(deploy.BIG_BROTHER_OFF.new_line, "#define USE_BIG_BROTHER 0")

    def test_coders_is_the_only_env_without_a_backup(self) -> None:
        self.assertEqual([name for name, env in deploy.ENVS.items() if not env.backup], ["coders"])

    def test_real_ports_are_tagged_and_need_the_deploy_branch(self) -> None:
        for name in ("live", "4k", "test", "coders"):
            self.assertEqual(deploy.ENVS[name].tag_prefix, name + "-")
            self.assertTrue(deploy.ENVS[name].require_branch)

    def test_test_targets_are_untagged_and_allow_any_branch(self) -> None:
        for name in ("zzz-forge-test", "zzz-forge-test-4k"):
            self.assertEqual(deploy.ENVS[name].dir_name, "zzz-forge-test")
            self.assertIsNone(deploy.ENVS[name].tag_prefix)
            self.assertFalse(deploy.ENVS[name].require_branch)

    def test_port_dir_guard_rejects_anything_but_a_plain_name(self) -> None:
        for bad in ("../etc", "a/b", "", "Live", "a b", "x;rm"):
            env = deploy.Env("bad", bad, deploy.CYAN, backup=True, tag_prefix=None)
            with self.assertRaises(deploy.DeployError):
                deploy.port_dir(env)


class ArgumentsTest(unittest.TestCase):
    def parse(self, *argv: str):
        with contextlib.redirect_stderr(io.StringIO()):
            return deploy.build_parser().parse_args(argv)

    def assert_usage_error(self, *argv: str) -> None:
        with self.assertRaises(SystemExit) as caught:
            self.parse(*argv)
        self.assertEqual(caught.exception.code, 2)

    def test_parses_env_login_and_port_in_order(self) -> None:
        args = self.parse("deploy", "live", "someone@example.org", "2222")

        self.assertEqual((args.env, args.login, args.port, args.dry_run),
                         ("live", ("someone", "example.org"), 2222, False))

    def test_dry_run_flag(self) -> None:
        self.assertTrue(self.parse("deploy", "4k", "someone@example.org", "2222", "--dry-run").dry_run)

    def test_every_argument_is_required(self) -> None:
        self.assert_usage_error()
        self.assert_usage_error("deploy")
        self.assert_usage_error("deploy", "live")
        self.assert_usage_error("deploy", "live", "someone@example.org")

    def test_unknown_env_is_rejected(self) -> None:
        self.assert_usage_error("deploy", "prod", "someone@example.org", "2222")

    def test_login_needs_exactly_one_at_with_both_sides(self) -> None:
        for bad in ("example.org", "a@b@c", "@example.org", "someone@"):
            self.assert_usage_error("deploy", "live", bad, "2222")

    def test_port_must_be_a_valid_number(self) -> None:
        for bad in ("ssh", "0", "70000", "-1", "22.0"):
            self.assert_usage_error("deploy", "live", "someone@example.org", bad)


# ---------------------------------------------------------------------------------------------
# Help files
# ---------------------------------------------------------------------------------------------

CONSTS_SNIPPET = """int help_summary_length = 2;

struct help_index_summary help_content[] = {
    { "general", "General information", "text/help_tbl", 0, 0, 0, 0 },
    { "specializations", "Becoming an expert in a field of your choice", "text/spec_tbl", 0, 0, 0,
        0 },
};

const char* unrelated = "text/not_a_chapter";
"""


class HelpFilesTest(unittest.TestCase):
    def test_selects_tracked_help_tables_and_the_help_page(self) -> None:
        tracked = ["lib/text/help", "lib/text/help_tbl", "lib/text/help_tbl.old", "lib/text/motd",
                   "lib/text/new_tbl", "lib/text/sub/deep_tbl", "src/other_tbl"]

        self.assertEqual(deploy.help_files(tracked), ["help", "help_tbl", "new_tbl"])


class HelpChaptersTest(unittest.TestCase):
    def test_reads_the_text_entries_of_help_content(self) -> None:
        self.assertEqual(deploy.help_chapters(CONSTS_SNIPPET), ["help_tbl", "spec_tbl"])

    def test_missing_table_is_an_error(self) -> None:
        with self.assertRaises(deploy.DeployError):
            deploy.help_chapters("int nothing_here;")

    def test_the_real_consts_cpp_lists_nine_chapters(self) -> None:
        text = (deploy.REPO_ROOT / "src" / "consts.cpp").read_text(errors="replace")

        self.assertEqual(deploy.help_chapters(text), ["help_tbl", "spel_tbl", "pray_tbl", "skil_tbl", "spec_tbl",
                                                      "wizh_tbl", "shap_tbl", "scr_tbl", "msdp_tbl"])


class HelpFormatTest(unittest.TestCase):
    def test_well_formed_file_passes(self) -> None:
        self.assertEqual(deploy.check_help_format("help_tbl", "KILL HIT\n\nStart a fight.\n#\nFLEE\nRun.\n#~\n"), [])

    def test_separator_with_trailing_space_passes(self) -> None:
        self.assertEqual(deploy.check_help_format("help_tbl", "A\ntext\n# \nB\ntext\n#~"), [])

    def test_line_starting_with_hash_fails_and_names_the_line(self) -> None:
        problems = deploy.check_help_format("help_tbl", "COLOR\nHex values must look like\n#RRGGBB.\n#~\n")

        self.assertEqual(len(problems), 1)
        self.assertIn("lib/text/help_tbl:3:", problems[0])
        self.assertIn("#RRGGBB.", problems[0])

    def test_missing_end_marker_fails(self) -> None:
        problems = deploy.check_help_format("spel_tbl", "A\ntext\n#\n")

        self.assertEqual(problems, ["lib/text/spel_tbl: does not end with a '#~' line"])

    def test_end_marker_before_the_end_fails(self) -> None:
        problems = deploy.check_help_format("help_tbl", "A\n#~\nB\n#~\n")

        self.assertEqual(len(problems), 1)
        self.assertIn("lib/text/help_tbl:2:", problems[0])


class CheckHelpTablesTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.repo = Path(temp.name)
        (self.repo / "src").mkdir()
        (self.repo / "lib" / "text").mkdir(parents=True)
        (self.repo / "src" / "consts.cpp").write_text(CONSTS_SNIPPET)

    def test_reports_bad_chapters_and_untracked_chapters(self) -> None:
        (self.repo / "lib" / "text" / "help_tbl").write_text("A\n#oops\n#~\n")

        problems = deploy.check_help_tables(self.repo, ["lib/text/help_tbl"])

        self.assertEqual(len(problems), 2)
        self.assertIn("lib/text/help_tbl:2:", problems[0])
        self.assertIn("lib/text/spec_tbl: listed in src/consts.cpp", problems[1])

    def test_clean_chapters_pass(self) -> None:
        for name in ("help_tbl", "spec_tbl"):
            (self.repo / "lib" / "text" / name).write_text("A\ntext\n#~\n")

        self.assertEqual(deploy.check_help_tables(self.repo, ["lib/text/help_tbl", "lib/text/spec_tbl"]), [])


# ---------------------------------------------------------------------------------------------
# The local checkout
# ---------------------------------------------------------------------------------------------


class GitRepoTestCase(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.repo = Path(temp.name)
        environment = mock.patch.dict(os.environ, {
            "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "test@example.org",
            "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "test@example.org",
        })
        environment.start()
        self.addCleanup(environment.stop)
        self.run_git("init", "-q", "-b", "release-frodo")
        self.write("src/game.cpp", "int main() {}\n")
        self.write("src/.gitignore", "*.o\n.remember/\n")
        self.write("lib/text/help_tbl", "A\n#~\n")
        self.write("lib/text/motd", "Welcome.\n")
        self.commit("initial")
        self.checkout = deploy.Checkout(self.repo)

    def run_git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True, capture_output=True, text=True).stdout

    def write(self, relative: str, text: str) -> None:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def commit(self, message: str) -> str:
        self.run_git("add", "-A")
        self.run_git("commit", "-q", "-m", message)
        return self.run_git("rev-parse", "HEAD").strip()


class CheckCheckoutTest(GitRepoTestCase):
    def test_clean_checkout_on_the_deploy_branch_passes(self) -> None:
        self.assertEqual(deploy.check_checkout(self.repo, deploy.ENVS["live"]), [])

    def test_uncommitted_change_stops(self) -> None:
        self.write("src/game.cpp", "int main() { return 1; }\n")

        with self.assertRaisesRegex(deploy.DeployError, "uncommitted"):
            deploy.check_checkout(self.repo, deploy.ENVS["live"])

    def test_ignored_object_file_in_src_stops(self) -> None:
        self.write("src/game.o", "binary")

        with self.assertRaisesRegex(deploy.DeployError, "src/game.o"):
            deploy.check_checkout(self.repo, deploy.ENVS["live"])

    def test_untracked_source_file_in_src_stops(self) -> None:
        self.write("src/new_feature.cpp", "// not committed\n")

        with self.assertRaisesRegex(deploy.DeployError, "src/new_feature.cpp"):
            deploy.check_checkout(self.repo, deploy.ENVS["live"])

    def test_dot_directories_in_src_and_files_outside_src_pass(self) -> None:
        self.write("src/.remember/now.md", "notes\n")
        self.write("notes.txt", "scratch\n")

        self.assertEqual(deploy.check_checkout(self.repo, deploy.ENVS["live"]), [])

    def test_other_branch_stops_a_real_port(self) -> None:
        self.run_git("checkout", "-q", "-b", "feat/x")

        with self.assertRaisesRegex(deploy.DeployError, "feat/x"):
            deploy.check_checkout(self.repo, deploy.ENVS["live"])

    def test_other_branch_only_warns_for_a_test_target(self) -> None:
        self.run_git("checkout", "-q", "-b", "feat/x")

        warnings = deploy.check_checkout(self.repo, TEST_ENV)

        self.assertEqual(len(warnings), 1)
        self.assertIn("feat/x", warnings[0])


class CheckoutTest(GitRepoTestCase):
    def test_head_returns_sha_and_subject(self) -> None:
        sha = self.run_git("rev-parse", "HEAD").strip()

        self.assertEqual(self.checkout.head(), (sha, "initial"))

    def test_help_files_come_from_git(self) -> None:
        self.write("lib/text/untracked_tbl", "A\n#~\n")

        self.assertEqual(self.checkout.help_files(), ["help_tbl"])

    def test_dry_run_does_not_pull(self) -> None:
        # There is no remote, so a pull would fail.
        self.assertEqual(self.checkout.prepare(deploy.ENVS["live"], dry_run=True), [])

    def test_real_run_pulls_on_the_deploy_branch(self) -> None:
        with self.assertRaisesRegex(deploy.DeployError, "git pull --ff-only failed"):
            self.checkout.prepare(deploy.ENVS["live"], dry_run=False)

    def test_test_target_on_another_branch_does_not_pull(self) -> None:
        self.run_git("checkout", "-q", "-b", "feat/x")

        self.assertEqual(len(self.checkout.prepare(TEST_ENV, dry_run=False)), 1)


class TagNamingTest(unittest.TestCase):
    DAY = datetime.date(2026, 9, 13)

    def test_first_tag_of_the_day(self) -> None:
        self.assertEqual(deploy.next_tag_name("test-", self.DAY, ["test-2026-09-12"]), "test-2026-09-13")

    def test_second_and_third_tags_of_the_day(self) -> None:
        self.assertEqual(deploy.next_tag_name("test-", self.DAY, ["test-2026-09-13"]), "test-2026-09-13-2")
        self.assertEqual(deploy.next_tag_name("test-", self.DAY, ["test-2026-09-13", "test-2026-09-13-2"]),
                         "test-2026-09-13-3")

    def test_previous_tag_is_the_latest_date_and_suffix_for_the_prefix(self) -> None:
        tags = ["live-2026-09-11", "live-2026-09-13", "live-2026-09-13-2", "live-2026-09-13-10",
                "test-2026-09-20", "live-hotfix", "4k-2026-10-01"]

        self.assertEqual(deploy.previous_tag("live-", tags), "live-2026-09-13-10")
        self.assertIsNone(deploy.previous_tag("coders-", tags))

    def test_help_changes_line(self) -> None:
        self.assertEqual(deploy.help_changes_line(None, []), "Help changes: first tagged deploy")
        self.assertEqual(deploy.help_changes_line("test-2026-09-13", []), "Help changes since test-2026-09-13: none")
        self.assertEqual(deploy.help_changes_line("test-2026-09-13", ["help_tbl", "shap_tbl"]),
                         "Help changes since test-2026-09-13: help_tbl, shap_tbl")


class CreateTagTest(GitRepoTestCase):
    def tag_contents(self, name: str) -> str:
        return self.run_git("tag", "-l", "--format=%(contents)", name)

    def test_first_tag_records_env_dir_sha_and_first_deploy(self) -> None:
        sha = self.run_git("rev-parse", "HEAD").strip()

        name = self.checkout.create_tag(deploy.ENVS["test"], sha, ["help_tbl"], datetime.date(2026, 9, 13))

        self.assertEqual(name, "test-2026-09-13")
        contents = self.tag_contents(name)
        self.assertIn(f"Deployed {sha} to test (/rots/dev-building4802).", contents)
        self.assertIn("Help changes: first tagged deploy", contents)
        self.assertNotIn("someone", contents)

    def test_later_tag_lists_only_help_files_changed_since_the_previous_tag(self) -> None:
        env = deploy.ENVS["test"]
        first = self.run_git("rev-parse", "HEAD").strip()
        self.checkout.create_tag(env, first, ["help_tbl"], datetime.date(2026, 9, 13))
        self.write("lib/text/help_tbl", "A\nnew text\n#~\n")
        self.write("lib/text/motd", "Changed on purpose.\n")
        second = self.commit("help update")

        name = self.checkout.create_tag(env, second, ["help_tbl"], datetime.date(2026, 9, 14))

        self.assertEqual(name, "test-2026-09-14")
        self.assertIn("Help changes since test-2026-09-13: help_tbl", self.tag_contents(name))

    def test_same_day_redeploy_gets_a_suffix_and_no_help_changes(self) -> None:
        env = deploy.ENVS["test"]
        sha = self.run_git("rev-parse", "HEAD").strip()
        self.checkout.create_tag(env, sha, ["help_tbl"], datetime.date(2026, 9, 13))

        name = self.checkout.create_tag(env, sha, ["help_tbl"], datetime.date(2026, 9, 13))

        self.assertEqual(name, "test-2026-09-13-2")
        self.assertIn("Help changes since test-2026-09-13: none", self.tag_contents(name))


# ---------------------------------------------------------------------------------------------
# Remote commands, run locally with sh against a fake port directory
# ---------------------------------------------------------------------------------------------


class RemoteCommandTestCase(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "port"
        for sub in ("src", "bin", "lib/text"):
            (self.root / sub).mkdir(parents=True)
        (self.root / "src" / "game.cpp").write_text("old source\n")
        (self.root / "src" / "game.o").write_text("old object\n")
        (self.root / "lib" / "text" / "help_tbl").write_text("old help\n#~\n")

    def sh(self, command: str) -> subprocess.CompletedProcess:
        local = command.replace(deploy.port_dir(TEST_ENV), str(self.root))
        self.assertNotIn("/rots/", local)
        return subprocess.run(["sh", "-c", local], capture_output=True, text=True)


class MissingDirsCommandTest(RemoteCommandTestCase):
    def test_passes_when_src_bin_and_lib_text_exist(self) -> None:
        self.assertEqual(self.sh(deploy.missing_dirs_command(TEST_ENV)).returncode, 0)

    def test_names_a_missing_directory(self) -> None:
        (self.root / "bin").rmdir()

        result = self.sh(deploy.missing_dirs_command(TEST_ENV))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing directory:", result.stdout)
        self.assertIn("/bin", result.stdout)


@unittest.skipIf(os.geteuid() == 0, "root can write everything")
class UnwritableCommandTest(RemoteCommandTestCase):
    def test_prints_nothing_when_everything_is_writable(self) -> None:
        result = self.sh(deploy.unwritable_command(TEST_ENV, ["help_tbl", "not_there_tbl"]))

        self.assertEqual((result.returncode, result.stdout), (0, ""))

    def test_lists_unwritable_source_and_help_files(self) -> None:
        (self.root / "src" / "game.cpp").chmod(0o444)
        (self.root / "lib" / "text" / "help_tbl").chmod(0o444)

        output = self.sh(deploy.unwritable_command(TEST_ENV, ["help_tbl"])).stdout

        self.assertIn(f"{self.root}/src/game.cpp", output)
        self.assertIn(f"{self.root}/lib/text/help_tbl", output)


class ChownCommandTest(unittest.TestCase):
    def test_is_valid_sh_and_targets_the_ssh_user(self) -> None:
        command = deploy.chown_command(TEST_ENV, "someone", ["help_tbl"])

        self.assertEqual(subprocess.run(["sh", "-n", "-c", command]).returncode, 0)
        self.assertIn("sudo chown -R someone /rots/zzz-forge-test/src /rots/zzz-forge-test/bin", command)
        self.assertIn("sudo chown someone /rots/zzz-forge-test/lib/text", command)


class BackupCommandTest(RemoteCommandTestCase):
    def test_copies_src_and_help_files_keeping_timestamps(self) -> None:
        stamp = time.time() - 3600
        os.utime(self.root / "src" / "game.o", (stamp, stamp))

        result = self.sh(deploy.backup_command(TEST_ENV, ["help_tbl", "not_there_tbl"]))

        self.assertEqual(result.returncode, 0, result.stderr)
        backup = self.root / "src" / "backup"
        self.assertEqual((backup / "game.cpp").read_text(), "old source\n")
        self.assertEqual(int((backup / "game.o").stat().st_mtime), int(stamp))
        self.assertEqual((backup / "lib-text" / "help_tbl").read_text(), "old help\n#~\n")
        self.assertFalse((backup / "lib-text" / "not_there_tbl").exists())
        self.assertFalse((self.root / "src" / "backup.new").exists())

    def test_second_run_replaces_the_backup_without_nesting(self) -> None:
        self.assertEqual(self.sh(deploy.backup_command(TEST_ENV, ["help_tbl"])).returncode, 0)
        (self.root / "src" / "backup" / "stale.txt").write_text("from an older backup\n")
        (self.root / "src" / "game.cpp").write_text("newer source\n")

        self.assertEqual(self.sh(deploy.backup_command(TEST_ENV, ["help_tbl"])).returncode, 0)

        backup = self.root / "src" / "backup"
        self.assertEqual((backup / "game.cpp").read_text(), "newer source\n")
        self.assertFalse((backup / "stale.txt").exists())
        self.assertFalse((backup / "backup").exists())

    @unittest.skipIf(os.geteuid() == 0, "root can read everything")
    def test_failed_copy_keeps_the_previous_backup(self) -> None:
        self.assertEqual(self.sh(deploy.backup_command(TEST_ENV, ["help_tbl"])).returncode, 0)
        (self.root / "src" / "backup" / "marker.txt").write_text("previous backup\n")
        unreadable = self.root / "src" / "secret.cpp"
        unreadable.write_text("x\n")
        unreadable.chmod(0o000)
        self.addCleanup(unreadable.chmod, 0o644)

        result = self.sh(deploy.backup_command(TEST_ENV, ["help_tbl"]))

        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.root / "src" / "backup" / "marker.txt").exists())

    def test_revert_restores_source_and_help_files(self) -> None:
        (self.root / "src" / "Makefile").write_text("all:\n\ttouch ../bin/ageland\n")
        self.assertEqual(self.sh(deploy.backup_command(TEST_ENV, ["help_tbl"])).returncode, 0)
        (self.root / "src" / "game.cpp").write_text("broken new source\n")
        (self.root / "lib" / "text" / "help_tbl").write_text("broken new help\n")

        result = self.sh(deploy.revert_command(TEST_ENV))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "src" / "game.cpp").read_text(), "old source\n")
        self.assertEqual((self.root / "lib" / "text" / "help_tbl").read_text(), "old help\n#~\n")
        self.assertFalse((self.root / "src" / "lib-text").exists())

    def test_rejects_an_unsafe_env(self) -> None:
        with self.assertRaises(deploy.DeployError):
            deploy.backup_command(deploy.Env("bad", "../etc", deploy.CYAN, backup=True, tag_prefix=None), [])


class SourceEditCommandTest(RemoteCommandTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.header = self.root / "src" / "big_brother.h"
        self.header.write_text("#ifndef USE_BIG_BROTHER\n#define USE_BIG_BROTHER 1\n#endif\n")
        self.command = deploy.source_edit_command(TEST_ENV, deploy.BIG_BROTHER_OFF)

    def test_turns_big_brother_off(self) -> None:
        result = self.sh(self.command)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.header.read_text(), "#ifndef USE_BIG_BROTHER\n#define USE_BIG_BROTHER 0\n#endif\n")

    def test_stops_when_the_line_is_already_changed(self) -> None:
        self.sh(self.command)

        result = self.sh(self.command)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected exactly one line: #define USE_BIG_BROTHER 1", result.stdout)

    def test_stops_without_editing_when_the_line_appears_twice(self) -> None:
        original = "#define USE_BIG_BROTHER 1\n#define USE_BIG_BROTHER 1\n"
        self.header.write_text(original)

        self.assertNotEqual(self.sh(self.command).returncode, 0)
        self.assertEqual(self.header.read_text(), original)


class BuildCommandTest(RemoteCommandTestCase):
    def test_passes_when_make_rebuilds_the_binary(self) -> None:
        (self.root / "src" / "Makefile").write_text("clean:\n\trm -f *.o\nall:\n\ttouch ../bin/ageland\n")

        result = self.sh(deploy.build_command(TEST_ENV))

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / "src" / "game.o").exists())

    def test_fails_when_the_binary_is_stale(self) -> None:
        (self.root / "src" / "Makefile").write_text("clean:\n\t@true\nall:\n\t@true\n")
        binary = self.root / "bin" / "ageland"
        binary.write_text("old")
        os.utime(binary, (time.time() - 3600, time.time() - 3600))

        result = self.sh(deploy.build_command(TEST_ENV))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not rebuilt", result.stdout)

    def test_fails_when_make_fails(self) -> None:
        (self.root / "src" / "Makefile").write_text("clean:\n\t@true\nall:\n\t@false\n")

        self.assertNotEqual(self.sh(deploy.build_command(TEST_ENV)).returncode, 0)


class SftpBatchTest(unittest.TestCase):
    def test_uploads_src_then_each_help_file(self) -> None:
        batch = deploy.sftp_batch(TEST_ENV, Path("/home/me/RotS"), ["help", "help_tbl"])

        self.assertEqual(batch, "\n".join([
            'lcd "/home/me/RotS/src"',
            'cd "/rots/zzz-forge-test/src"',
            "put -r *",
            'lcd "/home/me/RotS/lib/text"',
            'cd "/rots/zzz-forge-test/lib/text"',
            'put "help"',
            'put "help_tbl"',
        ]) + "\n")


# ---------------------------------------------------------------------------------------------
# Running commands
# ---------------------------------------------------------------------------------------------


class SshRunnerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.runner = deploy.SshRunner(SERVER, Path("/tmp/rots-deploy-test"))

    def test_connect_opens_a_master_connection(self) -> None:
        self.assertEqual(self.runner.connect_args(), ["ssh", "-M", "-S", "/tmp/rots-deploy-test/ssh-master", "-fN",
                                                      "-p", "2222", "someone@example.org"])

    def test_remote_reuses_the_master_and_can_request_a_tty(self) -> None:
        self.assertEqual(self.runner.remote_args("make"), ["ssh", "-S", "/tmp/rots-deploy-test/ssh-master", "-p", "2222",
                                                           "someone@example.org", "make"])
        self.assertEqual(self.runner.remote_args("sudo true", tty=True)[3], "-t")

    def test_sftp_reuses_the_master(self) -> None:
        self.assertEqual(self.runner.sftp_args(Path("/tmp/b")), ["sftp", "-o",
                         "ControlPath=/tmp/rots-deploy-test/ssh-master", "-P", "2222", "-b", "/tmp/b",
                         "someone@example.org"])

    def test_sftp_writes_the_batch_file_and_runs_it(self) -> None:
        with tempfile.TemporaryDirectory() as work_dir:
            runner = deploy.SshRunner(SERVER, Path(work_dir))
            with mock.patch.object(deploy, "run_command") as run:
                runner.sftp("put -r *\n")

            batch_path = Path(work_dir) / "upload.sftp"
            run.assert_called_once_with(runner.sftp_args(batch_path), "sftp upload")
            self.assertEqual(batch_path.read_text(), "put -r *\n")

    def test_close_only_runs_after_connecting(self) -> None:
        with mock.patch.object(deploy.subprocess, "run") as run:
            self.runner.close()
            run.assert_not_called()
            self.runner.connected = True
            self.runner.close()
            run.assert_called_once()
            self.assertFalse(self.runner.connected)


class RunCommandTest(unittest.TestCase):
    def test_returns_captured_output(self) -> None:
        self.assertEqual(deploy.run_command([sys.executable, "-c", "print('hello')"], "hello", capture=True), "hello\n")

    def test_failure_raises_with_status_and_output(self) -> None:
        with self.assertRaisesRegex(deploy.DeployError, "thing exited with status 3: oops"):
            deploy.run_command([sys.executable, "-c", "import sys; print('oops'); sys.exit(3)"], "thing", capture=True)


if __name__ == "__main__":
    unittest.main()
