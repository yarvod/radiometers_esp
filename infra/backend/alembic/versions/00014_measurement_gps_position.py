"""add gps position to measurements

Revision ID: 00014
Revises: 00013
Create Date: 2026-05-07 00:10:00.000000
"""

from alembic import op
import sqlalchemy as sa


revision = "00014"
down_revision = "00013"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column("measurements", sa.Column("gps_lat", sa.Float(), nullable=True))
    op.add_column("measurements", sa.Column("gps_lon", sa.Float(), nullable=True))
    op.add_column("measurements", sa.Column("gps_alt", sa.Float(), nullable=True))
    op.add_column("measurements", sa.Column("gps_fix_quality", sa.Integer(), nullable=True))
    op.add_column("measurements", sa.Column("gps_satellites", sa.Integer(), nullable=True))
    op.add_column("measurements", sa.Column("gps_fix_age_ms", sa.BigInteger(), nullable=True))


def downgrade() -> None:
    op.drop_column("measurements", "gps_fix_age_ms")
    op.drop_column("measurements", "gps_satellites")
    op.drop_column("measurements", "gps_fix_quality")
    op.drop_column("measurements", "gps_alt")
    op.drop_column("measurements", "gps_lon")
    op.drop_column("measurements", "gps_lat")
