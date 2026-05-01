"""use bigint for millisecond timestamps

Revision ID: 00011_bigint_timestamp_ms
Revises: 00010_device_gps_configs
Create Date: 2026-05-01 00:10:00.000000
"""

from alembic import op
import sqlalchemy as sa


revision = "00011"
down_revision = "00010"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.alter_column(
        "measurements",
        "timestamp_ms",
        existing_type=sa.Integer(),
        type_=sa.BigInteger(),
        existing_nullable=True,
    )
    op.alter_column(
        "error_events",
        "timestamp_ms",
        existing_type=sa.Integer(),
        type_=sa.BigInteger(),
        existing_nullable=True,
    )


def downgrade() -> None:
    op.alter_column(
        "error_events",
        "timestamp_ms",
        existing_type=sa.BigInteger(),
        type_=sa.Integer(),
        existing_nullable=True,
    )
    op.alter_column(
        "measurements",
        "timestamp_ms",
        existing_type=sa.BigInteger(),
        type_=sa.Integer(),
        existing_nullable=True,
    )
