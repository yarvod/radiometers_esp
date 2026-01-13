from app.worker import device_from_topic, parse_iso, parse_mqtt
from app.core.config import Settings


def test_parse_iso_defaults():
    result = parse_iso(None)
    assert result.tzinfo is not None


def test_parse_iso_with_z():
    result = parse_iso("2024-03-10T12:00:00Z")
    assert result.isoformat().startswith("2024-03-10T12:00:00")


def test_device_from_topic():
    assert device_from_topic("dev1/measure") == "dev1"
    assert device_from_topic("dev2/state") == "dev2"
    assert device_from_topic("") is None


def test_parse_mqtt_default():
    settings = Settings(mqtt_url="mqtt://localhost:1883")
    host, port = parse_mqtt(settings)
    assert host == "localhost"
    assert port == 1883
