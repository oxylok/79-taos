# SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
# SPDX-License-Identifier: MIT
import re

def duration_from_timestamp(timestamp : int) -> str:
    seconds, nanoseconds = divmod(timestamp, 1_000_000_000)
    minutes, seconds = divmod(seconds, 60)
    hours, minutes = divmod(minutes, 60)
    days, hours = divmod(hours, 24)
    return (f"{days}d " if days > 0 else "") + f"{hours:02}:{minutes:02}:{seconds:02}.{nanoseconds:09d}"

def timestamp_from_duration(duration: str) -> int:
    match = re.match(
        r'(?:(\d+)d\s+)?(\d{2}):(\d{2}):(\d{2})\.(\d{9})$', duration.strip()
    )
    if not match:
        raise ValueError(f"Invalid duration format: {duration}")

    days, hours, minutes, seconds, nanoseconds = match.groups()
    days = int(days) if days else 0
    hours = int(hours)
    minutes = int(minutes)
    seconds = int(seconds)
    nanoseconds = int(nanoseconds)

    total_seconds = (((days * 24 + hours) * 60 + minutes) * 60) + seconds
    return total_seconds * 1_000_000_000 + nanoseconds
        
def normalize(lower, upper, value):
    return (max(min(value, upper), lower) + upper) / (upper - lower)