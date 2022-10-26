# Wayland compositor running on top of an X serer.

# Copyright (C) 2022 to various contributors.

# This file is part of 12to11.

# 12to11 is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.

# 12to11 is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.

# You should have received a copy of the GNU General Public License
# along with 12to11.  If not, see <https://www.gnu.org/licenses/>.

BEGIN {
  print "#define DrmModifiersList \\"
};

/^#define[[:space:]]+(I915|DRM)_FORMAT_MOD_[[:alnum:]_]+[[:space:]]+fourcc_mod_code(.*)$/ {
  printf ("  { \"%s\", %s }, \\\n", $2, $2);
}

/^#define[[:space:]]+AMD_FMT_MOD_[[:alnum:]_]+[[:space:]]+(0x[[:xdigit:]]+|[:digit:]+)$/ {
  printf ("  { \"%s\", %s | AMD_FMT_MOD }, \\\n", $2, $2);
}

END {
  printf ("  { NULL, 0 },\n");
}

# Broadcom, ARM, etc modifiers not supported!  Patches welcome.
