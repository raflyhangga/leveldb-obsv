#!/usr/bin/env bash
# Matches env_->NowMicros(): gettimeofday -> microseconds since UNIX epoch
python3 -c "import time; print(int(time.time() * 1_000_000))"
