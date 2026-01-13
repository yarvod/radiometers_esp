from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    database_url: str = "postgresql+asyncpg://postgres:postgres@db:5432/radiometer"
    mqtt_url: str = "mqtt://mosquitto:1883"
    mqtt_user: str | None = None
    mqtt_password: str | None = None
    mqtt_measure_topic: str = "+/measure"
    mqtt_state_topic: str = "+/state"
    access_token_cookie: str = "access_token"
    access_token_header: str = "Authorization"

    model_config = SettingsConfigDict(env_prefix="APP_", env_file=".env", extra="ignore")
