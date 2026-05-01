"""add device gps configs

Revision ID: 00010_device_gps_configs
Revises: 00009_sounding_exports
Create Date: 2026-05-01 00:00:00.000000
"""

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql


revision = "00010"
down_revision = "00009"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "device_gps_configs",
        sa.Column("device_id", sa.String(length=64), sa.ForeignKey("devices.id"), primary_key=True),
        sa.Column("has_gps", sa.Boolean(), nullable=False, server_default=sa.text("false")),
        sa.Column("rtcm_types", postgresql.JSONB(astext_type=sa.Text()), nullable=False, server_default=sa.text("'[1004, 1006, 1033]'::jsonb")),
        sa.Column("mode", sa.String(length=32), nullable=False, server_default="base_time_60"),
        sa.Column("actual_mode", sa.Text(), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.func.now(), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), server_default=sa.func.now(), nullable=False),
    )
    op.alter_column("device_gps_configs", "has_gps", server_default=None)
    op.alter_column("device_gps_configs", "rtcm_types", server_default=None)
    op.alter_column("device_gps_configs", "mode", server_default=None)


def downgrade() -> None:
    op.drop_table("device_gps_configs")
