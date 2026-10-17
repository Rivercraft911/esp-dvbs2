# radio_s3 dry run in QEMU

This ESP-IDF app runs radio_s3's dry run, the bank sequence every radio run uses, at the 40 and
80 MS/s bank geometries, and checks every played sample and every switch's shared lead and tail.
It never touches Wi-Fi, the PHY, the DAC or bank ownership. QEMU cycle counts are not timing,
so the paced run is left to the board (`dvbpace`).

From the repository root, after activating ESP-IDF v6.1:

```sh
idf.py -C tests/radio_qemu build
cd tests/radio_qemu/build
python -m esptool --chip esp32s3 merge-bin -o flash.bin --fill-flash-size 4MB @flash_args
cd ../../..
qemu-system-xtensa -nographic -machine esp32s3 \
  -drive file=tests/radio_qemu/build/flash.bin,if=mtd,format=raw \
  -serial file:notes/radio-qemu.log -monitor none
/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 tests/test_radio.py notes/radio-qemu.log
```

Stop QEMU after `QEMU-END` appears in the log.
