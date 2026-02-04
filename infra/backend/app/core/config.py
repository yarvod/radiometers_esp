from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    database_url: str = "postgresql+asyncpg://postgres:postgres@db:5432/radiometer"
    mqtt_url: str = "mqtt://mosquitto:1883"
    mqtt_user: str | None = None
    mqtt_password: str | None = None
    mqtt_measure_topic: str = "+/measure"
    mqtt_state_topic: str = "+/state"
    mqtt_error_topic: str = "+/error"
    access_token_cookie: str = "access_token"
    access_token_header: str = "Authorization"
    access_token_ttl_minutes: int = 720
    stations_url: str = "https://weather.uwyo.edu/wsgi/sounding_json"
    stations_request_timeout: float = 35.0
    stations_connect_timeout: float = 4.0
    stations_read_timeout: float = 30.0
    stations_user_agent: str = "Radiometer/1.0"
    redis_url: str = "redis://redis:6379/0"

    model_config = SettingsConfigDict(env_prefix="APP_", env_file=".env", extra="ignore")
