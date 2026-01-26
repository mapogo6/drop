# drop

drop is a compact C project providing a TFTP-style client and daemon. It builds with CMake and uses libevent (bundled via vcpkg in this workspace).

## Features
- Small footprint TFTP client and daemon

## Building
Requires CMake and a C compiler.

From the project root:

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release
```

Built binaries are placed under the `build` output tree (e.g. `build/Debug/drop`, `build/Debug/dropd` depending on configuration).

To install (optional):

```bash
cmake --install . --config Release
```

## Running
After building, run the client or daemon binary from the build output:

```bash
./build/Debug/drop    # client binary (path depends on build config)
./build/Debug/dropd   # daemon binary
```

## Contributing
Contributions are welcome. Open issues or PRs with clear descriptions and small, focused changes.

## License
This project is distributed under the MIT License. See the `LICENSE` file for details.
