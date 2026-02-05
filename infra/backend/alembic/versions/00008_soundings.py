"""add soundings and schedules

Revision ID: 00008_soundings
Revises: 00007_stations_internal_id
Create Date: 2026-02-04
"""

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "00008"
down_revision = "00007"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "soundings",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("station_id", sa.String(length=36), sa.ForeignKey("stations.id"), nullable=False),
        sa.Column("sounding_time", sa.DateTime(timezone=True), nullable=False),
        sa.Column("station_name", sa.Text(), nullable=True),
        sa.Column("columns", postgresql.JSONB(), nullable=False, server_default=sa.text("'[]'::jsonb")),
        sa.Column("rows", postgresql.JSONB(), nullable=False, server_default=sa.text("'[]'::jsonb")),
        sa.Column("units", postgresql.JSONB(), nullable=False, server_default=sa.text("'{}'::jsonb")),
        sa.Column("raw_text", sa.Text(), nullable=False),
        sa.Column("row_count", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("fetched_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.UniqueConstraint("station_id", "sounding_time", name="uq_soundings_station_time"),
    )
    op.create_index("ix_soundings_station_id", "soundings", ["station_id"])
    op.create_index("ix_soundings_sounding_time", "soundings", ["sounding_time"])
    op.create_index("ix_soundings_fetched_at", "soundings", ["fetched_at"])

    op.create_table(
        "sounding_jobs",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("station_id", sa.String(length=36), sa.ForeignKey("stations.id"), nullable=False),
        sa.Column("status", sa.String(length=16), nullable=False),
        sa.Column("start_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("end_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("step_hours", sa.Integer(), nullable=False),
        sa.Column("total", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("done", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("error", sa.Text(), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
    )
    op.create_index("ix_sounding_jobs_station_id", "sounding_jobs", ["station_id"])
    op.create_index("ix_sounding_jobs_status", "sounding_jobs", ["status"])

    op.create_table(
        "sounding_schedule",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("station_id", sa.String(length=36), sa.ForeignKey("stations.id"), nullable=False),
        sa.Column("enabled", sa.Boolean(), nullable=False, server_default=sa.text("true")),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.UniqueConstraint("station_id", name="uq_sounding_schedule_station"),
    )
    op.create_index("ix_sounding_schedule_station_id", "sounding_schedule", ["station_id"])

    op.create_table(
        "sounding_schedule_config",
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column("interval_hours", sa.Integer(), nullable=False, server_default="3"),
        sa.Column("offset_hours", sa.Integer(), nullable=False, server_default="2"),
        sa.Column("updated_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
    )
    op.execute("INSERT INTO sounding_schedule_config (id, interval_hours, offset_hours) VALUES (1, 3, 2)")


def downgrade() -> None:
    op.drop_table("sounding_schedule_config")
    op.drop_index("ix_sounding_schedule_station_id", table_name="sounding_schedule")
    op.drop_table("sounding_schedule")
    op.drop_index("ix_sounding_jobs_status", table_name="sounding_jobs")
    op.drop_index("ix_sounding_jobs_station_id", table_name="sounding_jobs")
    op.drop_table("sounding_jobs")
    op.drop_index("ix_soundings_fetched_at", table_name="soundings")
    op.drop_index("ix_soundings_sounding_time", table_name="soundings")
    op.drop_index("ix_soundings_station_id", table_name="soundings")
    op.drop_table("soundings")
