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

/^\/\/==/ {
  if (in_program)
    {
      in_program = 0;
      print ";"
    }
  else
    {
      match ($0, /^\/\/== ([[:alnum:] ]+)/, array)
      in_program = ignore_line = 1;

      # Start a new shader; first, escape the name by replacing white
      # space with underscores.
      gsub (/[[:space:]]/, "_", array[1])

      # Next, make everything lowercase.
      array[1] = tolower (array[1]);

      printf "static const char *%s =\n", array[1]
    }
}

{
  if (ignore_line)
    ignore_line = 0;
  else if (in_program)
    {
      # Escape characters that can occur in regular GLSL programs but
      # must be escaped in C strings.
      string = $0
      gsub (/[\\"]/, "\\\\&", string)
      printf "  \"%s\\n\"\n", string
    }
}
