"""Exercise the shipped CLI using Python's independent, strict JSON decoder."""

import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile


def reject_constant(value):
    raise ValueError(f"non-JSON numeric constant: {value}")


def bytes_value(encoded):
    if encoded["encoding"] == "utf8":
        return encoded["value"].encode("utf-8")
    assert encoded["encoding"] == "hex"
    return bytes.fromhex(encoded["value"])


def main():
    tool, fixture = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="sub0log-json receiver ") as directory:
        subprocess.run([fixture, directory], check=True, capture_output=True, timeout=20)

        def run(*args):
            return subprocess.run([tool, *args, directory], capture_output=True, timeout=20)

        result = run("--format", "jsonl", "--stats")
        assert result.returncode == 0, result.stderr
        # Decode stdout strictly, then parse every physical line independently.
        records = [json.loads(line, parse_constant=reject_constant)
                   for line in result.stdout.decode("utf-8").splitlines()]
        assert len(records) == 8
        assert b"8 record(s)" in result.stderr
        assert b"unreadable 0 byte(s)" in result.stderr
        assert b"undecodable 0 record(s)" in result.stderr
        for record in records:
            assert record["schema"] == 1 and record["kind"] == "message"
            for name in ("process_id", "thread_id", "site_id", "correlation_id",
                         "mono_ns", "aligned_ns"):
                assert isinstance(record[name], str) and record[name].isdigit()
            assert int(record["correlation_id"]) == 9007199254740993
            assert int(record["process_id"]) > 0 and int(record["thread_id"]) > 0
            assert record["source_line"] > 0
            assert bytes_value(record["file"]).endswith(b"fixture.cpp")

        by_format = {bytes_value(r["format"]): r for r in records}
        integers = by_format[b"integers {} {} {} {} {} {} {} {}"]["args"]
        assert [a["type"] for a in integers] == list(range(2, 10))
        assert [a["value"] for a in integers] == [
            "-128", "255", "-32768", "65535", "-2147483648", "4294967295",
            "-9223372036854775808", "18446744073709551615"]
        scalars = by_format[b"scalars {} {} {}"]["args"]
        assert scalars[0] == {"type": 1, "value": True}
        assert scalars[1] == {"type": 12, "value": {"encoding": "hex", "value": "00"}}
        assert scalars[2]["type"] == 14 and int(scalars[2]["value"]) > 0
        floats = by_format[b"floats {} {} {} {} {} {} {} {}"]["args"]
        assert [a["type"] for a in floats] == [10, 11, 11, 11, 11, 11, 10, 10]
        assert [a["value"] for a in floats] == [1.25, 2.5, "inf", "-inf", "nan", -0.0, "inf", "nan"]
        assert math.copysign(1.0, floats[5]["value"]) == -1.0
        binary = by_format[b"bytes {}"]
        assert binary["args"][0]["type"] == 13
        assert bytes_value(binary["args"][0]["value"]) == b'\x00\n\r\t"\\\x1b\x80\xff'
        assert binary["message"]["encoding"] == "hex"
        unicode = by_format[b"unicode {}"]
        assert bytes_value(unicode["args"][0]["value"]) == "é😀".encode("utf-8")
        assert unicode["message"]["encoding"] == "utf8"
        invalid = by_format[b"\xffinvalid-format"]
        assert invalid["format"]["encoding"] == invalid["message"]["encoding"] == "hex"
        truncated = by_format[b"truncated {}"]
        assert truncated["truncated"] is True
        assert bytes_value(truncated["args"][0]["value"]) == b"z" * 4096
        assert bytes_value(truncated["subsystem_name"]) == b'receiver\n"test'

        assert run().stdout == run("--format", "text").stdout
        for args, expected in ((["--level", "warning"], 1), (["--subsystem", "7"], 7),
                               (["--correlation", "9007199254740993"], 8),
                               (["--correlation", "9007199254740992"], 0)):
            filtered = run("--format", "jsonl", *args)
            assert filtered.returncode == 0, filtered.stderr
            assert len([json.loads(line) for line in filtered.stdout.splitlines()]) == expected
        for args, message in ((["--format", "xml"], b"unknown format"),
                               (["--format"], b"unknown format"),
                               (["--format", "jsonl", "--follow"], b"remove --follow"),
                               (["--follow", "--format", "jsonl"], b"remove --follow")):
            invalid_result = run(*args)
            assert invalid_result.returncode == 2 and message in invalid_result.stderr
            assert not invalid_result.stdout
        missing_value = subprocess.run([tool, "--format"], capture_output=True, timeout=20)
        assert missing_value.returncode == 2 and b"needs a value" in missing_value.stderr
        missing_input = subprocess.run([tool, "--format", "jsonl", str(pathlib.Path(directory) / "absent")],
                                       capture_output=True, timeout=20)
        assert missing_input.returncode == 1 and not missing_input.stdout
        assert b"nothing readable" in missing_input.stderr
        bad_segment = pathlib.Path(directory) / "bad.s0l"
        bad_segment.write_bytes(b"not a segment")
        partial = run("--format", "jsonl", "--stats")
        assert partial.returncode == 0
        assert len([json.loads(line) for line in partial.stdout.splitlines()]) == 8
        assert b"not a readable segment" in partial.stderr and b"8 record(s)" in partial.stderr
        bad_segment.unlink()
        if pathlib.Path("/dev/full").exists():
            with open("/dev/full", "wb") as full:
                failed_output = subprocess.run([tool, "--format", "jsonl", "--stats", directory],
                                               stdout=full, stderr=subprocess.PIPE, timeout=20)
            assert failed_output.returncode == 1
            assert b"JSONL output" in failed_output.stderr
        elif os.name == "nt":
            failed_output = subprocess.Popen([tool, "--format", "jsonl", "--stats", directory],
                                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            # Close the receiving end, so the child's real stdout pipe fails.
            failed_output.stdout.close()
            failed_output.stdout = None
            _, stderr = failed_output.communicate(timeout=20)
            assert failed_output.returncode == 1, stderr
            assert b"JSONL output" in stderr
    print("JSONL snapshot receiver passed: typed values, encodings, filters and failures")


if __name__ == "__main__":
    main()
