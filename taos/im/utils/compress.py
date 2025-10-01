# SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
# SPDX-License-Identifier: MIT
import zlib, lz4.frame
import pybase64
import msgspec
from typing import Literal
from concurrent.futures import ThreadPoolExecutor

compressors = {
    "zlib": zlib.compress,
    "lz4": lz4.frame.compress,
}
decompressors = {
    "zlib": zlib.decompress,
    "lz4": lz4.frame.decompress,
}

def compress(
    payload,
    level: int = 1,
    engine: Literal["zlib", "lz4"] = "lz4",
    version: int = 45,
) -> str | None:
    """
    Compress a payload using either JSON (legacy, version < 45)
    or Msgpack (version >= 45), wrapped in Base64 text.
    """
    try:
        if version < 45:
            raw = msgspec.json.encode(payload)
        else:
            raw = msgspec.msgpack.encode(payload)

        compressed = compressors[engine](raw, level)
        return pybase64.b64encode(compressed).decode("ascii")
    except Exception as ex:
        print(f"Failed to compress! {ex}")
        return None


def decompress(
    payload: str | dict,
    engine: Literal["zlib", "lz4"] = "lz4",
    version: int = 45,
) -> dict | None:
    """
    Decompress payload using the correct codec depending on version.
    - version < 45 → JSON
    - version >= 45 → Msgpack
    Supports Base64-encoded transport, and old dict container format.
    """
    try:
        if isinstance(payload, str):
            decoded = pybase64.b64decode(payload)
            raw = decompressors[engine](decoded)

            if version < 45:
                return msgspec.json.decode(raw)
            return msgspec.msgpack.decode(raw)

        else:
            # Legacy container with 'payload' and 'books'
            decoded_main = pybase64.b64decode(payload["payload"])
            raw_main = decompressors[engine](decoded_main)
            if version < 45:
                decompressed_payload = msgspec.json.decode(raw_main)
            else:
                decompressed_payload = msgspec.msgpack.decode(raw_main)

            if payload.get("books"):
                decoded_books = pybase64.b64decode(payload["books"])
                raw_books = decompressors[engine](decoded_books)
                if version < 45:
                    books = msgspec.json.decode(raw_books)
                else:
                    books = msgspec.msgpack.decode(raw_books)
            else:
                books = {}

            return {"books": books, **decompressed_payload}

    except Exception as ex:
        print(f"Failed to decompress! {ex}")
        return None


def compress_batch(axon_synapses: dict, compressed_books : str, level: int = 1, engine: str = "lz4", version: int = 45) -> dict:    
    for uid, axon_synapse in axon_synapses.items():
        axon_synapse.books = None
        dumped = axon_synapse.model_dump(mode='json')
        payload = {
            "accounts": dumped['accounts'],
            "notices": dumped['notices'],
            "config": dumped['config'],
            "response": dumped['response'],
        }
        axon_synapse.accounts = None
        axon_synapse.notices = None
        axon_synapse.config = None
        axon_synapse.response = None
        axon_synapse.compressed = {
            "books": compressed_books,
            "payload": compress(payload, level=level, engine=engine, version=version),
        }
    return axon_synapses

def batch_compress(
    axon_synapses: dict,
    compressed_books: str,
    batches: list[list[int]],
    level: int = 1,
    engine: str = "lz4",
    version: int = 45,
) -> dict:
    compressed_batches = []
    with ThreadPoolExecutor(max_workers=len(batches)) as pool:
        tasks = [
            pool.submit(compress_batch, {uid: axon_synapses[uid] for uid in batch}, compressed_books,  level, engine, version)
            for batch in batches
        ]
        for task in tasks:
            compressed_batches.append(task.result())
    compressed_synapses = {k: v for d in compressed_batches for k, v in d.items()}
    return compressed_synapses
