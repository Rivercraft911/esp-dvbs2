# Finite S3 receive

One ESP32-S3 transmitted changing DVB-S2 data at 2420 MHz; another captured it
through its own radio. Three 125 ms records at 16 MS/s each produced two exact consecutive
QPSK 1/2 frames through LeanDVB: six frames and 24,096 payload bytes,
zero errors after FEC.
An independent header/pilot receiver with an external LDPC decoder recovered
three complete frames from the first record, totaling 12,048 exact bytes.
Neither receiver used expected payload bits during recovery or appended samples.

The receiver uses three radio SRAM banks and preallocated GDMA with 64-byte
bursts to store raw samples in PSRAM. It stops before packing signed 8-bit I/Q
and downloading 4 MB over USB. Bank, deadline, sentinel, DMA, guard and download
checks passed. Ten earlier 100 ms quiet captures also passed.

Periodic gain-forcing calls caused brief disturbances every 20 ms. Removing
those calls preserved the initial gain setup and steady tone amplitude while
eliminating the observed spikes in the control capture.

Firmware, sample and payload hashes are in `runs.jsonl`. These are finite captures
with offline decoding. They do not establish sustained capture, on-chip FEC or
packet reception below 2.4 GHz.
