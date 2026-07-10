"""add meteo_readings and measurements.meteo_reading_id FK

Revision ID: 00021
Revises: 00020
Create Date: 2026-07-10
"""

from alembic import op
import sqlalchemy as sa


revision = "00021"
down_revision = "00020"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.execute("SET LOCAL lock_timeout = '5s'")

    op.create_table(
        "meteo_readings",
        sa.Column("id", sa.String(length=36), nullable=False),
        sa.Column("device_id", sa.String(length=64), nullable=False),
        sa.Column("timestamp", sa.DateTime(timezone=True), nullable=False),
        sa.Column("timestamp_ms", sa.BigInteger(), nullable=False),
        sa.Column("temp_c", sa.Float(), nullable=True),
        sa.Column("humidity_pct", sa.Float(), nullable=True),
        sa.Column("wind_speed_ms", sa.Float(), nullable=True),
        sa.Column("gust_speed_ms", sa.Float(), nullable=True),
        sa.Column("wind_dir_deg", sa.Integer(), nullable=True),
        sa.Column("pressure_hpa", sa.Float(), nullable=True),
        sa.Column("rainfall_mm", sa.Float(), nullable=True),
        sa.Column("light_lux", sa.Float(), nullable=True),
        sa.Column("uvi", sa.Float(), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.func.now(), nullable=False),
        sa.ForeignKeyConstraint(["device_id"], ["devices.id"], ondelete="CASCADE"),
        sa.PrimaryKeyConstraint("id"),
        sa.UniqueConstraint("device_id", "timestamp_ms", name="uq_meteo_reading_device_time"),
    )
    op.create_index("ix_meteo_readings_device_id", "meteo_readings", ["device_id"])
    op.create_index("ix_meteo_readings_device_time", "meteo_readings", ["device_id", "timestamp"])

    op.add_column(
        "measurements",
        sa.Column("meteo_reading_id", sa.String(length=36), nullable=True),
    )
    op.create_index("ix_measurements_meteo_reading_id", "measurements", ["meteo_reading_id"])
    op.create_foreign_key(
        "fk_measurements_meteo_reading_id",
        "measurements",
        "meteo_readings",
        ["meteo_reading_id"],
        ["id"],
        ondelete="SET NULL",
    )


def downgrade() -> None:
    op.drop_constraint("fk_measurements_meteo_reading_id", "measurements", type_="foreignkey")
    op.drop_index("ix_measurements_meteo_reading_id", table_name="measurements")
    op.drop_column("measurements", "meteo_reading_id")
    op.drop_index("ix_meteo_readings_device_time", table_name="meteo_readings")
    op.drop_index("ix_meteo_readings_device_id", table_name="meteo_readings")
    op.drop_table("meteo_readings")
