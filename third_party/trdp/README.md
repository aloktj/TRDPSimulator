# TRDP stack drop-in directory

Place unpacked TCNopen TRDP stack releases in subdirectories named after their version numbers, for example:

```
third_party/trdp/
├── 3.0.0.0/
│   ├── config/
│   ├── src/
│   └── ...
├── 2.1.0.0/
│   ├── config/
│   ├── src/
│   └── ...
└── 2.0.3.0/
    ├── config/
    ├── src/
    └── ...
```

Do not trim any files from the archive—the simulator's CMake build consumes the `src/` tree directly and compiles the required
libraries automatically. Override the lookup with `-DTRDP_<version>_ROOT=/absolute/path/to/stack` (or `-DTRDP_ROOT=...` for single
builds) if the stacks live elsewhere, and select a specific configuration file with `-DTRDPSimulator_TRDP_CONFIG=<config>` when
the default auto-detected profile is insufficient.
