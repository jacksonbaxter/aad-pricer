#!/usr/bin/env python3
"""Makes <dest> match <source> by content, and leaves alone what is the same.

    python3 -I scripts/sync_tree.py [--keep <name>]... <source> <dest>

What `rsync -rlc --delete` does, on any host with Python: a regular file is rewritten only when
its bytes differ, and then gets the current time, so unchanged files keep their mtimes and ninja
rebuilds no more than changed. Permissions follow the source. Links are recreated as links, never
followed. Anything in <dest> that <source> lacks is deleted. A top-level entry named by --keep is
neither copied, replaced nor deleted, on either side; scripts/verify_commits.sh keeps the data
link that way. Prints the counts. Exit 0 on success, 1 on an error, 2 on bad usage. Standard
library only.
"""
import argparse
import filecmp
import os
import shutil
import stat
import sys


def remove(path):
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    else:
        os.remove(path)


def mode_of(path):
    return stat.S_IMODE(os.lstat(path).st_mode)


class Sync:
    def __init__(self, source, dest, keep):
        self.source = source
        self.dest = dest
        self.keep = keep
        self.copied = self.unchanged = self.deleted = 0

    def run(self):
        if os.path.lexists(self.dest) and not (
            os.path.isdir(self.dest) and not os.path.islink(self.dest)
        ):
            raise OSError(f"{self.dest} exists and is not a directory")
        os.makedirs(self.dest, exist_ok=True)
        self.directory(self.source, self.dest, top=True)

    def directory(self, source, dest, top):
        names = sorted(os.listdir(source))
        if top:
            names = [name for name in names if name not in self.keep]
        wanted = set(names)
        for name in sorted(os.listdir(dest)):
            if name in wanted or (top and name in self.keep):
                continue
            remove(os.path.join(dest, name))
            self.deleted += 1
        for name in names:
            self.entry(os.path.join(source, name), os.path.join(dest, name))

    def entry(self, source, dest):
        info = os.lstat(source)
        if stat.S_ISLNK(info.st_mode):
            link = os.readlink(source)
            if os.path.islink(dest) and os.readlink(dest) == link:
                self.unchanged += 1
                return
            if os.path.lexists(dest):
                remove(dest)
            os.symlink(link, dest)
            self.copied += 1
        elif stat.S_ISDIR(info.st_mode):
            if os.path.lexists(dest) and (os.path.islink(dest) or not os.path.isdir(dest)):
                remove(dest)
            os.makedirs(dest, exist_ok=True)
            if mode_of(dest) != stat.S_IMODE(info.st_mode):
                os.chmod(dest, stat.S_IMODE(info.st_mode))
            self.directory(source, dest, top=False)
        elif stat.S_ISREG(info.st_mode):
            same = (
                os.path.isfile(dest)
                and not os.path.islink(dest)
                and filecmp.cmp(source, dest, shallow=False)
            )
            if same:
                self.unchanged += 1
            else:
                if os.path.lexists(dest):
                    remove(dest)
                shutil.copyfile(source, dest)
                self.copied += 1
            # chmod leaves the mtime alone, so a mode change alone rebuilds nothing.
            if mode_of(dest) != stat.S_IMODE(info.st_mode):
                os.chmod(dest, stat.S_IMODE(info.st_mode))
        else:
            raise OSError(f"{source} is neither a file, a directory nor a link")


def main():
    parser = argparse.ArgumentParser(description="Make a tree match another by content.")
    parser.add_argument("--keep", action="append", default=[], metavar="NAME")
    parser.add_argument("source")
    parser.add_argument("dest")
    arguments = parser.parse_args()
    for name in arguments.keep:
        if not name or "/" in name or name in (".", ".."):
            parser.error(f"--keep takes a top-level name, not {name!r}")
    source = os.path.abspath(arguments.source)
    dest = os.path.abspath(arguments.dest)
    if not os.path.isdir(source) or os.path.islink(source):
        parser.error(f"{arguments.source} is not a directory")
    sync = Sync(source, dest, set(arguments.keep))
    try:
        sync.run()
    except OSError as error:
        print(f"sync_tree: {error}", file=sys.stderr)
        return 1
    print(f"sync_tree: {sync.copied} copied, {sync.unchanged} unchanged, {sync.deleted} deleted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
