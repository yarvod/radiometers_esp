from __future__ import annotations

import asyncio
import json
import uuid
from datetime import datetime, timezone
from urllib.parse import urlparse

from aiomqtt import Client, MqttError
from dishka import make_async_container

from app.container import AppProvider
from app.core.config import Settings
from app.domain.entities import Measurement
from app.services.devices import DeviceService
from app.services.measurements import MeasurementService


def parse_iso(value: str | None) -> datetime:
    if not value:
        return datetime.now(timezone.utc)
    raw = value.strip()
    if raw.endswith("Z"):
        raw = raw[:-1] + "+00:00"
    try:
        return datetime.fromisoformat(raw)
    except ValueError:
        return datetime.now(timezone.utc)


def parse_mqtt(settings: Settings):
    parsed = urlparse(settings.mqtt_url)
    host = parsed.hostname or "localhost"
    port = parsed.port or 1883
    return host, port


def device_from_topic(topic: str) -> str | None:
    if not topic:
        return None
    parts = topic.split("/", 1)
    if not parts or not parts[0]:
        return None
    return parts[0]


async def handle_measurement(topic: str, payload: bytes, container) -> None:
    device_id = device_from_topic(topic)
    if not device_id:
        return
    try:
        data = json.loads(payload.decode("utf-8"))
    except json.JSONDecodeError:
        return

    timestamp = parse_iso(data.get("timestampIso"))
    timestamp_ms = data.get("timestampMs")
    temps = data.get("temps") or []
    if not isinstance(temps, list):
        temps = []

    measurement = Measurement(
        id=str(uuid.uuid4()),
        device_id=device_id,
        timestamp=timestamp,
        timestamp_ms=int(timestamp_ms) if isinstance(timestamp_ms, (int, float)) else None,
        adc1=float(data.get("adc1", 0.0)),
        adc2=float(data.get("adc2", 0.0)),
        adc3=float(data.get("adc3", 0.0)),
        temps=[float(v) for v in temps if isinstance(v, (int, float))],
        bus_v=float(data.get("busV", 0.0)),
        bus_i=float(data.get("busI", 0.0)),
        bus_p=float(data.get("busP", 0.0)),
        adc1_cal=float(data["adc1Cal"]) if isinstance(data.get("adc1Cal"), (int, float)) else None,
        adc2_cal=float(data["adc2Cal"]) if isinstance(data.get("adc2Cal"), (int, float)) else None,
        adc3_cal=float(data["adc3Cal"]) if isinstance(data.get("adc3Cal"), (int, float)) else None,
        log_use_motor=bool(data.get("logUseMotor", False)),
        log_duration=float(data.get("logDuration", 1.0)),
        log_filename=data.get("logFilename"),
    )

    async with container() as request_container:
        devices = await request_container.get(DeviceService)
        measurements = await request_container.get(MeasurementService)
        await devices.touch_device(device_id, timestamp)
        await measurements.add(measurement)


async def handle_state(topic: str, container) -> None:
    device_id = device_from_topic(topic)
    if not device_id:
        return
    async with container() as request_container:
        devices = await request_container.get(DeviceService)
        await devices.touch_device(device_id, datetime.now(timezone.utc))


async def run_worker() -> None:
    settings = Settings()
    host, port = parse_mqtt(settings)
    container = make_async_container(AppProvider())

    while True:
        try:
            async with Client(hostname=host, port=port, username=settings.mqtt_user, password=settings.mqtt_password) as client:
                await client.subscribe(settings.mqtt_measure_topic)
                await client.subscribe(settings.mqtt_state_topic)
                async with client.messages() as messages:
                    async for message in messages:
                        topic = str(message.topic)
                        if topic.endswith("/measure"):
                            await handle_measurement(topic, message.payload, container)
                        elif topic.endswith("/state"):
                            await handle_state(topic, container)
        except MqttError:
            await asyncio.sleep(2)


if __name__ == "__main__":
    asyncio.run(run_worker())
