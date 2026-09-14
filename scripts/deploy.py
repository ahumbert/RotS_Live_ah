#!/usr/bin/env python3
"""Deploy RotS source and help files to one port on the game server.

    scripts/deploy.py deploy <env> <user>@<host> <ssh-port> [--dry-run]

The login and ssh port are arguments with no defaults, so this file never records them.
Design: docs/superpowers/specs/2026-09-13-deploy-script-design.md
"""

import argparse
import datetime
import os
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, List, Optional, Sequence, Tuple


REPO_ROOT = Path(__file__).resolve().parent.parent
DEPLOY_BRANCH = "release-frodo"
HELP_DIR = "lib/text"
PORT_DIR_PATTERN = re.compile(r"^/rots/[a-z0-9-]+$")
# The ssh control socket has to fit in a Unix socket path (about 108 bytes), so it lives under
# /tmp rather than a possibly long $TMPDIR.
SOCKET_PARENT = "/tmp"

BOLD_RED = "1;31"
BOLD_MAGENTA = "1;35"
YELLOW = "33"
GREEN = "32"
CYAN = "36"


class DeployError(Exception):
    """A deploy step failed; the message says why."""


# ---------------------------------------------------------------------------------------------
# Environments and arguments
# ---------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class SourceEdit:
    path: str  # relative to src/
    old_line: str
    new_line: str


BIG_BROTHER_OFF = SourceEdit("big_brother.h", "#define USE_BIG_BROTHER 1", "#define USE_BIG_BROTHER 0")


@dataclass(frozen=True)
class Env:
    name: str
    dir_name: str
    color: str
    backup: bool
    tag_prefix: Optional[str]
    source_edits: Tuple[SourceEdit, ...] = ()
    require_branch: bool = True


ENVS = {
    env.name: env
    for env in (
        Env("live", "live-default3791", BOLD_RED, backup=True, tag_prefix="live-"),
        Env("4k", "live-pkarena4000", BOLD_MAGENTA, backup=True, tag_prefix="4k-",
            source_edits=(BIG_BROTHER_OFF,)),
        Env("test", "dev-building4802", YELLOW, backup=True, tag_prefix="test-"),
        # The coding port keeps no backups (docs/Running the Game.md).
        Env("coders", "dev-coding4810", GREEN, backup=False, tag_prefix="coders-"),
        # Test targets: never tagged, and deployable from a feature branch.
        Env("zzz-forge-test", "zzz-forge-test", CYAN, backup=True, tag_prefix=None,
            require_branch=False),
        Env("zzz-forge-test-4k", "zzz-forge-test", CYAN, backup=True, tag_prefix=None,
            source_edits=(BIG_BROTHER_OFF,), require_branch=False),
    )
}


def port_dir(env: Env) -> str:
    """The env's directory on the server, refusing anything that is not a plain /rots/<name>."""
    path = f"/rots/{env.dir_name}"
    if not PORT_DIR_PATTERN.match(path):
        raise DeployError(f"refusing to run remote commands against {path!r}")
    return path


@dataclass(frozen=True)
class Server:
    user: str
    host: str
    port: int

    @property
    def login(self) -> str:
        return f"{self.user}@{self.host}"


def parse_login(value: str) -> Tuple[str, str]:
    user, _, host = value.partition("@")
    if value.count("@") != 1 or not user or not host:
        raise argparse.ArgumentTypeError(f"expected <user>@<host>, got {value!r}")
    return user, host


def parse_port(value: str) -> int:
    if not value.isdigit() or not 1 <= int(value) <= 65535:
        raise argparse.ArgumentTypeError(f"expected an ssh port number, got {value!r}")
    return int(value)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="deploy.py", description="Deploy RotS to a port on the game server.")
    commands = parser.add_subparsers(dest="command", required=True)
    deploy_parser = commands.add_parser("deploy", help="upload, build, and tag one env")
    deploy_parser.add_argument("env", choices=list(ENVS))
    deploy_parser.add_argument("login", type=parse_login, metavar="user@host")
    deploy_parser.add_argument("port", type=parse_port, metavar="ssh-port")
    deploy_parser.add_argument("--dry-run", action="store_true",
        help="run the local checks and print every step; change nothing")
    return parser


# ---------------------------------------------------------------------------------------------
# Help files
# ---------------------------------------------------------------------------------------------

HELP_CHAPTER_PATTERN = re.compile(r'"text/([^"/]+)"')


def help_files(tracked: Iterable[str]) -> List[str]:
    """The lib/text files every deploy uploads: the help tables (*_tbl) and the plain HELP page."""
    names = []
    for path in tracked:
        directory, _, name = path.rpartition("/")
        if directory == HELP_DIR and (name == "help" or name.endswith("_tbl")):
            names.append(name)
    return sorted(names)


def help_chapters(consts_text: str) -> List[str]:
    """The help files the game indexes: the "text/<name>" entries of help_content[] in consts.cpp."""
    start = consts_text.find("help_content[]")
    if start < 0:
        raise DeployError("could not find help_content[] in src/consts.cpp")
    end = consts_text.find("};", start)
    return HELP_CHAPTER_PATTERN.findall(consts_text[start:end])


def check_help_format(name: str, text: str) -> List[str]:
    """Problems that would break build_help_index (modify.cpp) or do_help (act_info.cpp).

    Both treat any line starting with '#' as the end of an entry, and '#~' as the end of the file.
    """
    lines = text.split("\n")
    while lines and not lines[-1].strip():
        lines.pop()
    problems = []
    for number, line in enumerate(lines, start=1):
        marker = line.rstrip()
        if not line.startswith("#") or marker == "#" or (marker == "#~" and number == len(lines)):
            continue
        if marker == "#~":
            problems.append(f"{HELP_DIR}/{name}:{number}: '#~' before the end of the file hides every entry after it")
        else:
            problems.append(f"{HELP_DIR}/{name}:{number}: starts with '#', which ends the entry here: {line!r}")
    if not lines or lines[-1].rstrip() != "#~":
        problems.append(f"{HELP_DIR}/{name}: does not end with a '#~' line")
    return problems


def check_help_tables(repo: Path, tracked: Iterable[str]) -> List[str]:
    tracked = set(tracked)
    problems = []
    for chapter in help_chapters((repo / "src" / "consts.cpp").read_text(errors="replace")):
        path = f"{HELP_DIR}/{chapter}"
        if path not in tracked:
            problems.append(f"{path}: listed in src/consts.cpp help_content[] but not tracked in git")
            continue
        problems.extend(check_help_format(chapter, (repo / path).read_text(errors="replace")))
    return problems


# ---------------------------------------------------------------------------------------------
# The local checkout
# ---------------------------------------------------------------------------------------------

TAG_DATE_PATTERN = re.compile(r"^(\d{4}-\d{2}-\d{2})(?:-(\d+))?$")


def git(repo: Path, *args: str) -> str:
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise DeployError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def current_branch(repo: Path) -> str:
    return git(repo, "rev-parse", "--abbrev-ref", "HEAD").strip()


def check_checkout(repo: Path, env: Env) -> List[str]:
    """Stop on anything that would deploy the wrong files; return warnings worth showing."""
    if git(repo, "status", "--porcelain", "--untracked-files=no").strip():
        raise DeployError("the checkout has uncommitted changes; commit or stash them first")
    extras = []
    status = git(repo, "status", "--porcelain", "--ignored", "--untracked-files=all", "--", "src")
    for line in status.splitlines():
        path = line[3:].strip('"')
        # `put -r *` skips names starting with '.', such as src/.remember/.
        if not path.split("/")[1].startswith("."):
            extras.append(path)
    if extras:
        raise DeployError("src/ has untracked or ignored files that `put -r *` would upload: " + ", ".join(extras))
    branch = current_branch(repo)
    if branch == DEPLOY_BRANCH:
        return []
    message = f"the checkout is on {branch!r}, not {DEPLOY_BRANCH!r}"
    if env.require_branch:
        raise DeployError(message)
    return [message + "; deploying it without pulling because this is a test target"]


def next_tag_name(prefix: str, day: datetime.date, existing: Iterable[str]) -> str:
    existing = set(existing)
    base = f"{prefix}{day.isoformat()}"
    name, count = base, 1
    while name in existing:
        count += 1
        name = f"{base}-{count}"
    return name


def previous_tag(prefix: str, existing: Iterable[str]) -> Optional[str]:
    """The latest <prefix>YYYY-MM-DD[-N] tag, or None."""
    dated = []
    for tag in existing:
        match = TAG_DATE_PATTERN.match(tag[len(prefix):]) if tag.startswith(prefix) else None
        if match:
            dated.append((match.group(1), int(match.group(2) or 1), tag))
    return max(dated)[2] if dated else None


def help_changes_line(previous: Optional[str], changed: Sequence[str]) -> str:
    if previous is None:
        return "Help changes: first tagged deploy"
    return f"Help changes since {previous}: {', '.join(changed) if changed else 'none'}"


def tag_message(env: Env, sha: str, help_line: str) -> str:
    return f"Deployed {sha} to {env.name} ({port_dir(env)}).\n\n{help_line}\n"


class Checkout:
    """The local git checkout being deployed."""

    def __init__(self, repo: Path):
        self.repo = repo

    def prepare(self, env: Env, dry_run: bool) -> List[str]:
        warnings = check_checkout(self.repo, env)
        if not dry_run and current_branch(self.repo) == DEPLOY_BRANCH:
            git(self.repo, "pull", "--ff-only")
        return warnings

    def head(self) -> Tuple[str, str]:
        sha, _, subject = git(self.repo, "log", "-1", "--format=%H%x00%s").strip().partition("\0")
        return sha, subject

    def _tracked_help_dir(self) -> List[str]:
        return git(self.repo, "ls-files", "--", HELP_DIR).splitlines()

    def help_files(self) -> List[str]:
        return help_files(self._tracked_help_dir())

    def help_problems(self) -> List[str]:
        return check_help_tables(self.repo, self._tracked_help_dir())

    def create_tag(self, env: Env, sha: str, help_names: Sequence[str], day: datetime.date) -> str:
        existing = git(self.repo, "tag", "--list").split()
        previous = previous_tag(env.tag_prefix, existing)
        changed: List[str] = []
        if previous is not None:
            paths = [f"{HELP_DIR}/{name}" for name in help_names]
            diff = git(self.repo, "diff", "--name-only", previous, sha, "--", *paths)
            changed = [path.rpartition("/")[2] for path in diff.split()]
        name = next_tag_name(env.tag_prefix, day, existing)
        git(self.repo, "tag", "-a", name, sha, "-m", tag_message(env, sha, help_changes_line(previous, changed)))
        return name
