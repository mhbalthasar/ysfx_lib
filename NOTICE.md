# NOTICE

This repository contains a modified copy of ysfx:

- Upstream: https://github.com/JoepVanlier/ysfx
- Upstream license: Apache License 2.0 (see `LICENSE`)
- Upstream version at fork time: `0.0.47`

The local changes are limited to packaging ysfx as a shared C ABI library for
TuneLab and extending the build matrix to the platforms listed in
`docs/ysfx-lib.md`. The upstream copyright notices remain in the source files.

The vendored `thirdparty/WDL`, `thirdparty/dr_libs`, `thirdparty/stb`,
`thirdparty/json`, and `thirdparty/clap-juce-extensions` trees keep their
original licenses. `plugin_license/` contains the license texts that apply to
the upstream plugin target; the shared-library build does not compile that
target.
