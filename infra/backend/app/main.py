from __future__ import annotations

import logging

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from dishka import make_async_container
from dishka.integrations.fastapi import setup_dishka

from app.api.routes import auth_router, devices_router, measurements_router, stations_router, users_router
from app.container import AppProvider


def setup_logging() -> None:
    root = logging.getLogger()
    if not root.handlers:
        logging.basicConfig(
            level=logging.INFO,
            format="%(asctime)s | %(levelname)s | %(name)s | %(message)s",
        )
    else:
        root.setLevel(logging.INFO)


def create_app() -> FastAPI:
    setup_logging()
    app = FastAPI(title="Radiometer API")
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )
    app.include_router(auth_router, prefix="/api")
    app.include_router(users_router, prefix="/api")
    app.include_router(devices_router, prefix="/api")
    app.include_router(measurements_router, prefix="/api")
    app.include_router(stations_router, prefix="/api")

    container = make_async_container(AppProvider())
    setup_dishka(container, app)
    return app


app = create_app()
