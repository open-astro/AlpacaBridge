### Fixed (packaging)

- **`debian/copyright` records libgphoto2 and LibRaw** (issue #1065): the gphoto camera driver links both dynamically from the distro packages, so the file now names libgphoto2 (LGPL-2.1-or-later) and LibRaw (LGPL-2.1 or CDDL-1.0, with a `License: CDDL-1.0` paragraph) in its header `Comment:` field, and says that bundling libgphoto2 (issue #933) must extend the record and add the LGPL source offer.
