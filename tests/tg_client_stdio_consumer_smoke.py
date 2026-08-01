"""Exercise the pinned tg-client-stdio worker through its host-side API."""

from __future__ import annotations

import sys

from tg_client_stdio_worker.process import WorkerProcess, WorkerProcessConfig


def main() -> int:
    config = WorkerProcessConfig(
        args=["--mock"],
        executable=sys.executable,
    )
    with WorkerProcess(config) as client:
        hello = client.hello({"client_name": "optionx-consumer-smoke"})
        if hello.get("backend") != "mock":
            raise AssertionError(f"unexpected worker backend: {hello!r}")

        dialogs = client.dialogs()
        if not dialogs or dialogs[0].get("chat_id") != "-1001234567890":
            raise AssertionError(f"unexpected dialogs response: {dialogs!r}")

        messages: list[dict] = []
        summary = client.stream_messages(
            {"chat": "-10042"},
            messages.append,
        )
        if summary.messages != 2 or len(messages) != 2:
            raise AssertionError(
                f"unexpected export summary: {summary!r}, messages={messages!r}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
