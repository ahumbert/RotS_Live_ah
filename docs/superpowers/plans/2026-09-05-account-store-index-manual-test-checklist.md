# Account Store Index — Manual Test Checklist

For the merge of `feat/account-store` into `release-frodo` (merge commit `03f84a8`).
Design: `docs/superpowers/specs/2026-09-05-account-store-index-design.md`.

**What changed, in one line:** account lookups no longer walk `accounts/` — an in-memory index
answers them. Nothing on disk changed: same JSON, same filenames, same paths, same write order.

**What did NOT change, and is worth knowing before you start:** the JSON, the filenames, the paths,
and the order writes happen in. If you see a difference in any of those, that is a bug, not the
feature.

---

## Most of this is now scripted

Two server+client smoke scripts cover the client-observable items, run against a booted server:

```
export ROTS_SMOKE_EMAIL=... ROTS_SMOKE_PASSWORD=... ROTS_SMOKE_CHARACTER=...
python3 tools/smoke_account_index.py [port]                # sections 2, 3, 4, 5 (except the drill)
python3 tools/smoke_account_index_quarantine.py [port] [quarantined-email]
```

The fixture comes from the environment, not from a default: the scripts are in the repo and the
fixture password is not. `ROTS_SMOKE_CHARACTER` must be at `LEVEL_GRGOD` or above, since the
`account` command is immortal-only. On this machine the values are the `clauded3bugbot` fixture;
they are recorded in memory rather than here.

Both were run green in a throwaway worktree on 2026-09-06 against the merged tree, in exactly the
form committed: 19/19 on a healthy tree and 7/7 against a genuinely corrupted record.

The scripts are a floor, not a substitute — they prove the paths still work, not that the game feels
right. Section 1 (boot) and the quarantine drill's corrupt/reboot/restore steps stay manual, because
they need a server restart against a doctored account tree; the second script asserts the state
*after* you have done that.

---

## Before you start

- **Quit every character all the way out of the game between selection tests.** A character left
  in-game makes a later selection reconnect to it, which fakes the result and has burned smoke tests
  on this codebase before.
- **Do not run the quarantine drill (section 5) against live player data.** Use a copy.

---

## 1. Boot

- [ ] Server boots. The log shows `Account index: N account(s) indexed.`
- [ ] `N` matches `find lib/accounts -name account.json | wc -l`.
- [ ] No `record(s) quarantined` line on a healthy tree.

`log()` writes to **stderr**, not to `log/syslog` — read it wherever the port's stderr goes (under
`scripts/rots-docker.sh boot`, `docker logs <container>`).

## 2. Login

- [ ] Login by email address works, and feels instant.
- [ ] A wrong password is rejected; the right one then works.
- [ ] An **unknown** email says `No account exists for that email address.` and offers to create an
      account. This exact string is compared by `interpre.cpp` — if the wording changed, something is
      wrong.
- [ ] Account creation at a fresh address works end to end.

## 3. Roster and characters

- [ ] The roster lists the same characters it did before, in the same order.
- [ ] Selection by number works. Selection by full name works (case-insensitive).
- [ ] The sort/filter keys still behave (`a`/`l`/`c`/`s`, `w`/`r`/`t`/`m`).
- [ ] Link a character — it appears on the roster.
- [ ] Delete a character — it disappears, and stays gone across a logout and back in.

## 4. Saving — the one to be fussy about

- [ ] Play a linked character, quit cleanly, and confirm the save landed in the **account**
      directory (`accounts/<bucket>/<email>/<name>.character.json`), by modification time.
- [ ] The legacy `players/` file for that character was NOT written instead.

This is the path where a wrong answer is silently destructive, so check the file, not just that the
game said "saved".

## 5. Immortal surface (`LEVEL_GRGOD`)

- [ ] `account index` — reports the count and lists nothing quarantined on a healthy tree.
- [ ] `account index verify` — reports `Index agrees with disk.`
- [ ] `account index frobnicate` — prints usage rather than silently showing the summary.

### Quarantine drill — on a COPY of the account tree, never live

- [ ] Corrupt one `account.json` (e.g. `echo 'not json' > …`), boot.
- [ ] The server **boots** rather than exiting, and logs `1 record(s) quarantined` plus a per-record
      line naming the file and why.
- [ ] `account index` lists that record.
- [ ] `account index verify` still reports agreement — a quarantined record present on disk is not
      drift.
- [ ] Creating an account at a **different** address still works. (A quarantined record must not
      block creation game-wide.)
- [ ] Creating an account at the **quarantined** address is refused. (Its address is reserved — this
      is what stops a new account being written over a real player's record.)
- [ ] `account index off` is **refused** while anything is quarantined. This is deliberate: with the
      index off, one quarantined record makes every account-native character save write nothing.
- [ ] Restore the record; confirm byte-identical; reboot; `verify` clean again.

## 6. If something is wrong in production

1. `account index off` — falls every resolver back to the pre-change directory scans. It refuses
   while records are quarantined; if it refuses, move the quarantined files aside first.
2. If that is not enough, run the previous binary. No data migration is needed in either direction.

---

## Notes for whoever reads this later

- Boot refuses to continue past **5** unusable records. That threshold is a bug detector, not a
  corruption tolerance: the write path cannot produce a torn file, so many bad records at once means
  we shipped something, and stopping before players write on top of it is the recoverable outcome.
- Production has **no** legacy flat account records (`accounts/<bucket>/<name>.json`) — verified 0 on
  the live tree, 2026-09-05. The code still handles them because the retained fallback scans do.
- The fallback scans are meant to live for one release and then be deleted, taking the legacy-flat
  handling with them.
