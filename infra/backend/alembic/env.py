from __future__ import annotations

import asyncio
import sys
from pathlib import Path
from logging.config import fileConfig

from alembic import context
from sqlalchemy import pool
from sqlalchemy.engine import Connection
from sqlalchemy.ext.asyncio import async_engine_from_config

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from app.core.config import Settings
from app.db.base import Base
from app.db import models  # noqa: F401

config = context.config
fileConfig(config.config_file_name)

target_metadata = Base.metadata


def get_database_url() -> str:
    settings = Settings()
    return settings.database_url


def run_migrations_offline() -> None:
    url = get_database_url()
    context.configure(
        url=url,
        target_metadata=target_metadata,
        literal_binds=True,
        dialect_opts={"paramstyle": "named"},
        process_revision_directives=process_revision_directives,
    )
    with context.begin_transaction():
        context.run_migrations()


def do_run_migrations(connection: Connection) -> None:
    context.configure(
        connection=connection,
        target_metadata=target_metadata,
        process_revision_directives=process_revision_directives,
    )
    with context.begin_transaction():
        context.run_migrations()


async def run_migrations_online() -> None:
    config.set_main_option("sqlalchemy.url", get_database_url())
    connectable = async_engine_from_config(
        config.get_section(config.config_ini_section),
        prefix="sqlalchemy.",
        poolclass=pool.NullPool,
    )
    async with connectable.connect() as connection:
        await connection.run_sync(do_run_migrations)
    await connectable.dispose()


def _next_revision_id() -> str:
    versions_dir = Path(__file__).resolve().parent / "versions"
    max_rev = 0
    for path in versions_dir.glob("*.py"):
        stem = path.stem
        prefix = stem.split("_", 1)[0]
        if prefix.isdigit():
            max_rev = max(max_rev, int(prefix))
    return f"{max_rev + 1:05d}"


def process_revision_directives(context, revision, directives) -> None:
    if not directives:
        return
    script = directives[0]
    if getattr(script, "rev_id", None):
        return
    script.rev_id = _next_revision_id()


if context.is_offline_mode():
    run_migrations_offline()
else:
    asyncio.run(run_migrations_online())
