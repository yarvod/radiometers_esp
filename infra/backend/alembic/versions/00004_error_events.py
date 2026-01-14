"""add error events

Revision ID: 00004_error_events
Revises: 00003_temp_addresses
Create Date: 2026-01-14
"""

from alembic import op
import sqlalchemy as sa

revision = "00004"
down_revision = "00003"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "error_events",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("device_id", sa.String(length=64), sa.ForeignKey("devices.id"), nullable=False),
        sa.Column("timestamp", sa.DateTime(timezone=True), nullable=False),
        sa.Column("timestamp_ms", sa.Integer(), nullable=True),
        sa.Column("code", sa.String(length=64), nullable=False),
        sa.Column("severity", sa.String(length=16), nullable=False),
        sa.Column("message", sa.Text(), nullable=False),
        sa.Column("active", sa.Boolean(), nullable=False, server_default=sa.text("true")),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
    )
    op.create_index("ix_error_events_device_id", "error_events", ["device_id"])
    op.create_index("ix_error_events_timestamp", "error_events", ["timestamp"])
    op.create_index("ix_error_events_created_at", "error_events", ["created_at"])
    op.alter_column("error_events", "active", server_default=None)


def downgrade() -> None:
    op.drop_index("ix_error_events_created_at", table_name="error_events")
    op.drop_index("ix_error_events_timestamp", table_name="error_events")
    op.drop_index("ix_error_events_device_id", table_name="error_events")
    op.drop_table("error_events")
