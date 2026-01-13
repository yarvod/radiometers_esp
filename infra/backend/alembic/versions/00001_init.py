"""init

Revision ID: 00001
Revises: 
Create Date: 2026-01-13 22:10:00
"""

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql


revision = "00001"
down_revision = None
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "devices",
        sa.Column("id", sa.String(length=64), primary_key=True),
        sa.Column("display_name", sa.String(length=128), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()")),
        sa.Column("last_seen_at", sa.DateTime(timezone=True), nullable=True),
    )

    op.create_table(
        "users",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("username", sa.String(length=64), nullable=False),
        sa.Column("password_hash", sa.Text(), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()")),
    )
    op.create_index("ix_users_username", "users", ["username"], unique=True)

    op.create_table(
        "access_tokens",
        sa.Column("token", sa.String(length=128), primary_key=True),
        sa.Column("user_id", sa.String(length=36), sa.ForeignKey("users.id"), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()")),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
    )
    op.create_index("ix_access_tokens_user_id", "access_tokens", ["user_id"], unique=False)
    op.create_index("ix_access_tokens_expires_at", "access_tokens", ["expires_at"], unique=False)

    op.create_table(
        "measurements",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("device_id", sa.String(length=64), sa.ForeignKey("devices.id"), nullable=False),
        sa.Column("timestamp", sa.DateTime(timezone=True), nullable=False),
        sa.Column("timestamp_ms", sa.Integer(), nullable=True),
        sa.Column("adc1", sa.Float(), nullable=False),
        sa.Column("adc2", sa.Float(), nullable=False),
        sa.Column("adc3", sa.Float(), nullable=False),
        sa.Column("temps", postgresql.ARRAY(sa.Float()), nullable=False),
        sa.Column("bus_v", sa.Float(), nullable=False),
        sa.Column("bus_i", sa.Float(), nullable=False),
        sa.Column("bus_p", sa.Float(), nullable=False),
        sa.Column("adc1_cal", sa.Float(), nullable=True),
        sa.Column("adc2_cal", sa.Float(), nullable=True),
        sa.Column("adc3_cal", sa.Float(), nullable=True),
        sa.Column("log_use_motor", sa.Boolean(), server_default=sa.text("false")),
        sa.Column("log_duration", sa.Float(), server_default=sa.text("1.0")),
        sa.Column("log_filename", sa.Text(), nullable=True),
    )
    op.create_index("ix_measurements_device_id", "measurements", ["device_id"], unique=False)
    op.create_index("ix_measurements_timestamp", "measurements", ["timestamp"], unique=False)


def downgrade() -> None:
    op.drop_index("ix_measurements_timestamp", table_name="measurements")
    op.drop_index("ix_measurements_device_id", table_name="measurements")
    op.drop_table("measurements")
    op.drop_index("ix_access_tokens_expires_at", table_name="access_tokens")
    op.drop_index("ix_access_tokens_user_id", table_name="access_tokens")
    op.drop_table("access_tokens")
    op.drop_index("ix_users_username", table_name="users")
    op.drop_table("users")
    op.drop_table("devices")
