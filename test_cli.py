"""Independent integer/file-format oracle and failure-path checks (stdlib only)."""
import pathlib
import random
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())
rng = random.Random(42)


def run(*args, ok=True):
    result = subprocess.run([binary, *map(str, args)], capture_output=True)
    assert (result.returncode == 0) == ok, (args, result.stderr.decode())
    return result


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    source, output = root / "input", root / "output"
    for bits in (8, 16, 32, 64):
        width = bits // 8
        for signed in (False, True):
            low = -(1 << (bits - 1)) if signed else 0
            high = (1 << (bits - int(signed))) - 1
            # Cross the bucket-sort threshold and leave a non-vector-sized tail.
            values = [low, high, 0, 1] + [rng.randint(low, high) for _ in range(262147)]
            for endian in ("little", "big"):
                flags = ["--bits", bits, "--threads", 2]
                if signed:
                    flags += ["--signed"]
                if endian == "big":
                    flags += ["--big-endian"]
                raw = b"".join(v.to_bytes(width, endian, signed=signed) for v in values)
                expected = b"".join(v.to_bytes(width, endian, signed=signed) for v in sorted(values))
                source.write_bytes(raw)
                run("sort", *flags, source, output)
                assert output.read_bytes() == expected, (bits, signed, endian)
                run("sort", *flags, source, source)
                assert source.read_bytes() == expected, "in-place path replacement failed"
    source.write_bytes(b"")
    run("sort", source, output)
    assert output.read_bytes() == b""
    source.write_bytes(b"\x01")
    output.write_bytes(b"keep existing output")
    run("sort", "--bits", 32, source, output, ok=False)
    assert output.read_bytes() == b"keep existing output"
    run("sort", root / "missing", output, ok=False)
    run("sort", "--bits", 8, source, root / "missing" / "output", ok=False)
    run("sort", "--bits", 8, source, root, ok=False)
    assert not list(root.glob("*.tmp.*")), "temporary output leaked"
    for args in [[], ["sort"], ["sort", "--bits", "24"],
                 ["bench", "--count", "-1"], ["bench", "--count", "0"],
                 ["bench", "--threads", "257"], ["bench", "--threads", "4294967297"],
                 ["bench", "--repeats", "101"], ["bench", "--bits"],
                 ["bench", "--big-endian"], ["bench", "--unknown"]]:
        run(*args, ok=False)
    run("--help")
    run("bench", "--count", 1001, "--bits", 32, "--signed", "--repeats", 1)
print("All CLI tests passed.")
