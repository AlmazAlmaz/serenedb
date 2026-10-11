from __future__ import annotations

import os
import socket
import subprocess
import time
from pathlib import Path

import psycopg
import pytest

SERENED_BIN = os.environ.get(
    "SDB_DRV_SERENED_BIN",
    str(Path(__file__).resolve().parents[3] / "build" / "bin" / "serened"),
)

pytestmark = pytest.mark.skipif(
    not (Path(SERENED_BIN).is_file() and os.access(SERENED_BIN, os.X_OK)),
    reason=f"serened binary not found or not executable: {SERENED_BIN}",
)


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class _Server:
    def __init__(self, datadir: Path, listen: str | None = None) -> None:
        self.port = _free_port()
        if listen is None:
            listen = f"postgres://127.0.0.1:{self.port}"
        self.log = datadir.parent / f"serened-{self.port}.log"
        with self.log.open("w") as out:
            self.proc = subprocess.Popen(
                [SERENED_BIN, str(datadir), f"--listen={listen}"],
                stdout=out, stderr=subprocess.STDOUT)

    def wait_ready(self, timeout: float = 60.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                return False
            try:
                psycopg.connect(host="127.0.0.1", port=self.port,
                                user="postgres", dbname="postgres").close()
                return True
            except psycopg.OperationalError:
                time.sleep(0.2)
        return False

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=60)

    def output(self) -> str:
        return self.log.read_text()


def test_second_instance_on_the_same_datadir(tmp_path: Path) -> None:
    datadir = tmp_path / "data"
    first = _Server(datadir)
    try:
        assert first.wait_ready(), first.output()
        second = _Server(datadir)
        try:
            assert second.proc.wait(timeout=60) != 0, second.output()
            assert (f"database is locked by process {first.proc.pid}"
                    in second.output()), second.output()
        finally:
            second.stop()
    finally:
        first.stop()


def test_unknown_unix_socket_group(tmp_path: Path) -> None:
    server = _Server(tmp_path / "data",
                     f"postgres://{tmp_path / 'sdb.sock'}"
                     "?group=sdb_no_such_group")
    try:
        assert server.proc.wait(timeout=60) != 0, server.output()
        assert ("cannot find group 'sdb_no_such_group'"
                in server.output()), server.output()
    finally:
        server.stop()
