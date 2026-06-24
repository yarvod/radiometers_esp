"""add device has_meteo flag

Revision ID: 00019
Revises: 00018
Create Date: 2026-06-24
"""

from alembic import op
import sqlalchemy as sa


revision = "00019"
down_revision = "00018"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "devices",
        sa.Column("has_meteo", sa.Boolean(), nullable=False, server_default=sa.false()),
    )
    op.alter_column("devices", "has_meteo", server_default=None)


def downgrade() -> None:
    op.drop_column("devices", "has_meteo")
