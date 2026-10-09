# Known bugs

[Back to the README](../../README.md)

Bugs that are known and left for later, with what shows them.

## Replacing or reverting a whole tile can leave a few objects wrong

**Where:** pasting another version's tile in place with its objects (Versions, Compare), and Revert selection to
client.

**What happens:** objects whose bounds only graze the edge of the selection can be missed. Replacing Azeroth 32_48
with Ascension's version (`Azeroth_Asc`) leaves 3 of its objects missing and 2 of the map's objects behind; reverting it
restores 763 of the client's 766 objects. The terrain itself is right. Epoch's version of the same tile has no such
objects and comes out exact.

**Shown by:** `--compare-check <Data> Azeroth 32 48` on a client that has the Ascension maps (2 problems).

**Workaround:** delete or place the few objects on the tile's border by hand.

**Likely cause:** objects straddling the tile border are counted on one side of the comparison but not the other
(the added copies are clipped to loaded tiles; see the note in `src/ChecksVersions.cpp`).
