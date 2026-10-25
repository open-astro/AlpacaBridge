# OnStep Mount Resources

This directory holds the OnStepX protocol reference the AlpacaCore OnStep telescope driver is
checked against.

## Files

- **COMMAND_REFERENCE.md** - the OnStepX command reference, copied unmodified from
  [hjd1964/OnStepX](https://github.com/hjd1964/OnStepX) `docs/COMMAND_REFERENCE.md`
  - Commit: `097cc5797b4708e3787dd0b8bbbf81df66fdb5ff` (last changed upstream 2026-10-16)
  - sha256: `d88f5541af19a8f1812d9aea836f6c311d0ac5393e239cdee61090af23786ebf`
  - Covers OnStepX (firmware 10.x). The older OnStep firmware (4.x, branch `release-4.24`)
    documents its commands in the comments of `Command.ino` in
    [hjd1964/OnStep](https://github.com/hjd1964/OnStep); where the two differ for the commands
    the driver sends is listed in `.github/instructions/onstep.instructions.md`
    ("OnStep 4.x and OnStepX").
- **LICENSE** - the GNU General Public License, version 3, from the same OnStepX commit.

## Copyright and license

OnStepX is Copyright (C) 2021-2026 Howard Dutton and is free software under the GNU General
Public License, version 3 or (at your option) any later version. `COMMAND_REFERENCE.md` is part
of OnStepX and is distributed here unmodified under that license; its full text is in `LICENSE`.

## Updating

Replace `COMMAND_REFERENCE.md` with the file at a newer OnStepX commit, update the commit and
sha256 above and the pinned link in `.github/instructions/onstep.instructions.md`, and re-check the
"Commands this driver sends" table there against it. Do not edit the copy itself.
