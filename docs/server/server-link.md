# Server link

[Back to the README](../../README.md) · File > Server setup

The editor talks to an AzerothCore server two ways: its MySQL world database (spawns, paths, templates) and SOAP
(GM commands such as `.wp reload`).

## Setup

**File > Server setup** (it opens by itself for a project without a server):

1. Pick the AzerothCore server folder; **Read settings from worldserver.conf** fills the database and SOAP fields.
2. SOAP needs `SOAP.Enabled = 1` in `worldserver.conf` and a game account with GM level 3.
3. Save the profile. Passwords go to the Windows credential store; the rest to
   `%APPDATA%\wow-world-editor\profiles.json`. The project keeps only the profile's name, so projects can be shared
   without credentials.

The (?) markers link the matching AzerothCore wiki pages. The status bar shows whether the server is reachable.

## Server panel

A GM command console over SOAP: type a command (`server info`, `.wp reload 123`, ...) and see the reply. The
worldserver must be running.

## Problems

**Problems** lists what an export would refuse or fix: tiles failing the structure check, open edges between tiles,
missing assets, unknown ground effects, ids outside the project's ranges or other rows inside them, full or unset
ranges. Click one to go there. **Check for problems** (command palette) runs the checks again; they also run when the
project opens and on every export.
