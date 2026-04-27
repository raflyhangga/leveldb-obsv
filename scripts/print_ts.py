#!/usr/bin/env python3
import time

# Mirrors env_->NowMicros(): gettimeofday() -> microseconds since UNIX epoch
ts_us = int(time.time() * 1_000_000)
print(ts_us)
