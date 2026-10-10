# libmspub, PubToPDF branch

This is a fork of [libmspub](https://git.libreoffice.org/libmspub), the library that reads Microsoft Publisher
(.pub) files, with the changes made for [PubToPDF](https://pubtopdf.app) by Mrrp Software.

- Branch `pubtopdf`: upstream commit `652b2be` plus 16 commits, one per change.
- What each change does, and how sure we are about it: [OUR-PATCHES.md](OUR-PATCHES.md).
- Licence: MPL-2.0, same as upstream ([COPYING.MPL](COPYING.MPL)).

Patches 01 (32-bit length overflow) and 09 (metadata errors no longer fail the parse) are plain bug fixes that could
go upstream as they are.

Contact: hello@mrrpsoftware.com
