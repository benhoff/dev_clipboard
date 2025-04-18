# tests/test_parallel_read_rwlock.py
import os
import time
import threading
import pytest

DEV = "/dev/clipboard"

@pytest.fixture(scope="module")
def base_size():
    """Start at 2 MiB for single‐reader timing."""
    return 2 * 1024 * 1024   # 2 MiB

def rewrite_buffer(size):
    """Overwrite the clipboard with `size` bytes of 'A', always closing the FD."""
    fdw = os.open(DEV, os.O_RDWR | os.O_TRUNC)
    try:
        # allocate in one go; might throw OSError if too big
        os.write(fdw, b'A' * size)
    finally:
        os.close(fdw)

def reader(size, barrier=None):
    """Helper: seek to 0, barrier.wait(), then read <size> bytes."""
    if barrier:
        barrier.wait()
    fdr = os.open(DEV, os.O_RDONLY)
    try:
        os.lseek(fdr, 0, os.SEEK_SET)
        os.read(fdr, size)
    finally:
        os.close(fdr)

def test_parallel_read_speed(base_size):
    size = base_size
    threshold = 0.005           # want single read ≥5 ms
    max_size   = base_size * 8  # cap at 16 MiB to avoid OOM

    # Ramp the buffer until we get a measurable single‐reader time
    single_duration = 0.0
    while True:
        try:
            rewrite_buffer(size)
        except OSError:
            pytest.skip(f"Cannot allocate {size/(1024*1024):.0f} MiB buffer")

        start = time.time()
        reader(size)
        single_duration = time.time() - start

        if single_duration >= threshold or size >= max_size:
            break
        size *= 2

    if single_duration < threshold:
        pytest.skip(
            f"Even {size/(1024*1024):.0f} MiB reads too fast ({single_duration:.6f}s)"
        )

    # Barrier so both threads fire the read at the same moment
    barrier = threading.Barrier(2)

    t0 = time.time()
    t1 = threading.Thread(target=reader, args=(size, barrier))
    t2 = threading.Thread(target=reader, args=(size, barrier))
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    parallel_duration = time.time() - t0

    # With rw_semaphore allowing real parallel reads, should be <1.5× single
    assert parallel_duration < single_duration * 1.5, (
        f"Reads did not run concurrently: single={single_duration:.6f}s, "
        f"parallel={parallel_duration:.6f}s"
    )

