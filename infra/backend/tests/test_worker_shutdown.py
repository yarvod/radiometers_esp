import asyncio
import signal
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from app import worker


@pytest.mark.asyncio
@pytest.mark.parametrize("message_in_progress", [False, True])
async def test_sigterm_closes_worker_after_current_message(monkeypatch, message_in_progress):
    entered = asyncio.Event()
    processing = asyncio.Event()
    release_processing = asyncio.Event()
    queue = asyncio.Queue()
    callbacks = []
    container = SimpleNamespace(close=AsyncMock())

    class Messages:
        async def __anext__(self):
            return await queue.get()

    class Client:
        messages = Messages()

        def __init__(self, **kwargs):
            pass

        async def __aenter__(self):
            entered.set()
            return self

        async def __aexit__(self, *_):
            pass

        async def subscribe(self, *_):
            pass

    async def process(*_):
        processing.set()
        await release_processing.wait()

    settings = SimpleNamespace(
        mqtt_url="mqtt://localhost:1883", mqtt_user="user", mqtt_password="password",
        mqtt_measure_topic="+/measure", mqtt_state_topic="+/state", mqtt_error_topic="+/error",
    )
    monkeypatch.setattr(worker, "Settings", lambda: settings)
    monkeypatch.setattr(worker, "make_async_container", lambda *_: container)
    monkeypatch.setattr(worker, "Client", Client)
    handler = AsyncMock(side_effect=process)
    monkeypatch.setattr(worker, "handle_measurement", handler)
    monkeypatch.setattr(asyncio.get_running_loop(), "add_signal_handler", lambda sig, cb: callbacks.append((sig, cb)))

    task = asyncio.create_task(worker.run_worker())
    await asyncio.wait_for(entered.wait(), timeout=1)
    if message_in_progress:
        queue.put_nowait(SimpleNamespace(topic="device/measure", payload=b"{}"))
        await asyncio.wait_for(processing.wait(), timeout=1)
    assert callbacks[0][0] == signal.SIGTERM
    callbacks[0][1]()
    await asyncio.sleep(0)
    if message_in_progress:
        assert not task.done()
    release_processing.set()
    await asyncio.wait_for(task, timeout=1)
    assert handler.await_count == int(message_in_progress)
    container.close.assert_awaited_once()
