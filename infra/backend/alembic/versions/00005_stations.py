"""add stations

Revision ID: 00005_stations
Revises: 00004_error_events
Create Date: 2026-02-04
"""

from alembic import op
import sqlalchemy as sa

revision = "00005"
down_revision = "00004"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "stations",
        sa.Column("id", sa.String(length=64), primary_key=True),
        sa.Column("name", sa.String(length=128), nullable=True),
        sa.Column("lat", sa.Float(), nullable=True),
        sa.Column("lon", sa.Float(), nullable=True),
        sa.Column("src", sa.String(length=64), nullable=True),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("now()"), nullable=False),
    )
    op.create_index("ix_stations_updated_at", "stations", ["updated_at"])


def downgrade() -> None:
    op.drop_index("ix_stations_updated_at", table_name="stations")
    op.drop_table("stations")
