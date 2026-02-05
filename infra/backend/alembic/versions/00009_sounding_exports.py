"""add sounding export jobs

Revision ID: 00009_sounding_exports
Revises: 00008_soundings
Create Date: 2026-02-05
"""

from alembic import op
import sqlalchemy as sa
from sqlalchemy.dialects import postgresql

revision = "00009"
down_revision = "00008"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "sounding_export_jobs",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("station_id", sa.String(length=36), sa.ForeignKey("stations.id"), nullable=False),
        sa.Column("status", sa.String(length=16), nullable=False),
        sa.Column("sounding_ids", postgresql.JSONB(), nullable=False, server_default=sa.text("'[]'::jsonb")),
        sa.Column("total", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("done", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("error", sa.Text(), nullable=True),
        sa.Column("file_path", sa.Text(), nullable=True),
        sa.Column("file_name", sa.Text(), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
    )
    op.create_index("ix_sounding_export_jobs_station_id", "sounding_export_jobs", ["station_id"])
    op.create_index("ix_sounding_export_jobs_status", "sounding_export_jobs", ["status"])


def downgrade() -> None:
    op.drop_index("ix_sounding_export_jobs_status", table_name="sounding_export_jobs")
    op.drop_index("ix_sounding_export_jobs_station_id", table_name="sounding_export_jobs")
    op.drop_table("sounding_export_jobs")
