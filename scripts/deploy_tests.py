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


if __name__ == "__main__":
    unittest.main()
