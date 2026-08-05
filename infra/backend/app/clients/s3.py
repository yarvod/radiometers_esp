from __future__ import annotations

import asyncio
from typing import Protocol, Sequence
from urllib.parse import urlparse

from app.core.config import Settings
from app.domain.entities import S3ObjectDescriptor


class S3ObjectStore(Protocol):
    async def list_objects(self, bucket: str, prefix: str) -> Sequence[S3ObjectDescriptor]: ...

    async def get_object(self, bucket: str, object_key: str) -> bytes: ...


class MinioObjectStore:
    def __init__(self, settings: Settings) -> None:
        from minio import Minio

        endpoint, secure = _parse_endpoint(settings.s3_endpoint)
        if bool(settings.s3_access_key) != bool(settings.s3_secret_key):
            raise ValueError("APP_S3_ACCESS_KEY and APP_S3_SECRET_KEY must be configured together")
        self._client = Minio(
            endpoint,
            access_key=settings.s3_access_key,
            secret_key=settings.s3_secret_key,
            secure=secure,
            region=settings.s3_region,
            cert_check=settings.s3_cert_check,
        )
        self._max_object_bytes = settings.s3_max_object_bytes

    async def list_objects(self, bucket: str, prefix: str) -> Sequence[S3ObjectDescriptor]:
        return await asyncio.to_thread(self._list_objects, bucket, prefix)

    async def get_object(self, bucket: str, object_key: str) -> bytes:
        return await asyncio.to_thread(self._get_object, bucket, object_key)

    def _list_objects(self, bucket: str, prefix: str) -> list[S3ObjectDescriptor]:
        result: list[S3ObjectDescriptor] = []
        for item in self._client.list_objects(bucket, prefix=prefix, recursive=True):
            if not item.object_name or item.is_dir:
                continue
            result.append(
                S3ObjectDescriptor(
                    key=item.object_name,
                    etag=(item.etag or "").strip('"'),
                    last_modified=item.last_modified,
                    size=int(item.size or 0),
                )
            )
        result.sort(key=lambda item: item.key)
        return result

    def _get_object(self, bucket: str, object_key: str) -> bytes:
        response = self._client.get_object(bucket, object_key)
        try:
            payload = response.read(self._max_object_bytes + 1)
        finally:
            response.close()
            response.release_conn()
        if len(payload) > self._max_object_bytes:
            raise ValueError(
                f"S3 object exceeds APP_S3_MAX_OBJECT_BYTES={self._max_object_bytes}: {object_key}"
            )
        return payload


def _parse_endpoint(value: str) -> tuple[str, bool]:
    raw = value.strip()
    parsed = urlparse(raw if "://" in raw else f"http://{raw}")
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise ValueError("APP_S3_ENDPOINT must be an http(s) endpoint")
    if parsed.path not in {"", "/"}:
        raise ValueError("APP_S3_ENDPOINT must not contain a path")
    return parsed.netloc, parsed.scheme == "https"
