#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run --config=fast --config=raviole //private/google-modules/soc/gs:slider_dist "$@"
