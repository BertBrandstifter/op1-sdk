# my_tape

TODO: what it does for the player.

## Controls

- shift + insert: TODO

## Install (op1-sdk)

```sh
python3 tools/features.py install <this repository URL> --ref v0.1.0
python3 tools/compose.py --features my_tape --name my_tape ~/path/to/op1_246.op1
python3 tools/flash.py build/apps/my_tape/build/my_tape.op1 --eject     # te-boot upload mode
```

Firmware: OP-1 R246 (stock SHA-256 in feature.json). Status: experimental.
