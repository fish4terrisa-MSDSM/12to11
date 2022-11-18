#!/usr/bin/env bash
# Tests for the Wayland compositor running on top of an X serer.

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

pushd "$(dirname $0)"
declare -a standard_tests=(
    simple_test damage_test transform_test viewporter_test
    subsurface_test scale_test seat_test dmabuf_test
    xdg_activation_test single_pixel_buffer_test buffer_test
)

make -C . "${standard_tests[@]}"

export GLOBAL_SCALE=1
export OUTPUT_SCALE=1
exec 3< <(stdbuf -oL ../12to11 -printsocket)
read -u 3 WAYLAND_DISPLAY
export WAYLAND_DISPLAY

echo "Compositor started at ${WAYLAND_DISPLAY}"

for test_executable in "${standard_tests[@]}"
do
    echo "Running test ${test_executable}"

    if ./${test_executable}; then
	echo "${test_executable} completed successfully"
    else
	echo "${test_executable} failed; see its output for more details"
    fi
done

echo "Starting Xvfb at :27"

Xvfb :27 &
sleep 1
exec 4< <(DISPLAY=:27 stdbuf -oL ../12to11 -printsocket)
read -u 4 WAYLAND_DISPLAY
export WAYLAND_DISPLAY

declare -a vfb_tests=(
    select_test
)

make -C . "${vfb_tests[@]}" select_helper select_helper_multiple

echo "Compositor for vfb tests started at ${WAYLAND_DISPLAY}"

for test_executable in "${vfb_tests[@]}"
do
    echo "Running test ${test_executable}"

    if ./${test_executable}; then
	echo "${test_executable} completed successfully"
    else
	echo "${test_executable} failed; see its output for more details"
    fi
done

popd

trap 'jobs -p | xargs kill' EXIT
