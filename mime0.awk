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
  print "/* Automatically generated file.  Do not edit! */"
  print "#define DirectTransferMappings \\"
}

/  ([a-z]+\/[-+.[:alnum:]]+) / { # Leave 2 spaces before and 1 space after
  match ($0, /  ([a-z]+\/[-+.[:alnum:]]+) /, array)
  printf "	{ 0, \"%s\"},\\\n", array[1]
}

END {
  printf "\n"
}
