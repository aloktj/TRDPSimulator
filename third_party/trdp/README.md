# TRDP stack drop-in directory

Place unpacked TCNopen TRDP stack releases in subdirectories named after their version numbers, for example:

```
third_party/trdp/
├── 3.0.0.0/
│   ├── include/
│   └── lib/
├── 2.1.0.0/
│   ├── include/
│   └── lib/
└── 2.0.3.0/
    ├── include/
    └── lib/
```

The CMake build searches these locations when configuring the simulator. Override the lookup with `-DTRDP_<version>_ROOT=/absolute/path/to/stack` (or `-DTRDP_ROOT=...` for single builds) if the stacks live elsewhere.
